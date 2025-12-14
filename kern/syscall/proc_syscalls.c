// proc_syscalls.c
// Implementations of process-related system calls:
// getpid, waitpid, exit, fork, execv.

#include <types.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <copyinout.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <syscall.h>
#include "proc_syscalls.h"
#include "exec_support.h"

// Returns the pid of the current process in *retval.
// Never fails once curproc is valid.
int
sys_getpid(pid_t *retval)
{
    if (retval == NULL) {
        return EINVAL;
    }
    if (curproc == NULL) {
        return ESRCH;
    }

    *retval = curproc->p_pid;
    return 0;
}

// Waits for a specific child pid and retrieves its exit status.
// On success stores return status in *child_status and pid in *retval.
int
sys_waitpid(pid_t pid, int *child_status, int wait_options, int32_t *retval)
{
    if (retval == NULL) {
        return EINVAL;
    }
    if (curproc == NULL) {
        return ESRCH;
    }

    // A process cannot wait on itself.
    if (pid == curproc->p_pid) {
        return ECHILD;
    }

    // Status pointer must be valid and 4-byte aligned.
    if (child_status == NULL || ((vaddr_t)child_status % 4) != 0) {
        return EFAULT;
    }

    // Check child relationship.
    if (proc_is_child(curproc, pid) == -1) {
        return ECHILD;
    }

    // Options: only 0 or WNOHANG allowed.
    if (wait_options != 0 && wait_options != WNOHANG) {
        return EINVAL;
    }

    struct proc *child_proc = proc_search(pid);
    if (child_proc == NULL) {
        return ESRCH;
    }

    // If already exited and no threads remain, return immediately.
    if (child_proc->p_numthreads == 0) {
        *child_status = child_proc->p_status;
        *retval       = child_proc->p_pid;
        proc_destroy(child_proc);
        return 0;
    }

    // Blocking wait unless WNOHANG is used.
    if (wait_options == WNOHANG) {
        *child_status = 0;
        *retval       = pid;
        return 0;
    }

    lock_acquire(child_proc->p_locklock);
    cv_wait(child_proc->p_cv, child_proc->p_locklock);
    lock_release(child_proc->p_locklock);

    *child_status = child_proc->p_status;
    *retval       = child_proc->p_pid;
    proc_destroy(child_proc);

    return 0;
}

// Terminates the current process and notifies parent waiters.
// Does not return.
void
sys__exit(int exitcode)
{
    if (curproc == NULL) {
        thread_exit();
    }

    struct proc *p = curproc;
    p->p_status = _MKWAIT_EXIT(exitcode);

    proc_remthread(curthread);

    lock_acquire(p->p_locklock);
    cv_signal(p->p_cv, p->p_locklock);
    lock_release(p->p_locklock);

    thread_exit();
}

// Creates a duplicate process and initializes its execution context.
// Returns 0 in the child and child's pid in the parent.
int
sys_fork(struct trapframe *ctf, pid_t *retval)
{
    if (ctf == NULL || retval == NULL) {
        return EINVAL;
    }
    if (curproc == NULL) {
        return ESRCH;
    }

    int new_pid = pid_allocate();
    if (new_pid <= 0) {
        return ENPROC;
    }

    struct proc *child_proc = proc_create_runprogram(curproc->p_name);
    if (child_proc == NULL) {
        return ENOMEM;
    }

    int err = as_copy(curproc->p_addrspace, &child_proc->p_addrspace);
    if (err != 0) {
        proc_destroy(child_proc);
        return err;
    }

    struct trapframe *child_tf_copy = kmalloc(sizeof(struct trapframe));
    if (child_tf_copy == NULL) {
        proc_destroy(child_proc);
        return ENOMEM;
    }

    memmove(child_tf_copy, ctf, sizeof(struct trapframe));

    if (add_new_child(curproc, child_proc->p_pid) == -1) {
        kfree(child_tf_copy);
        proc_destroy(child_proc);
        return ENOMEM;
    }

    child_proc->parent_pid = curproc->p_pid;

    err = proc_register_pid((pid_t)new_pid, child_proc);
    if (err == -1) {
        kfree(child_tf_copy);
        proc_destroy(child_proc);
        return ENOMEM;
    }

    err = thread_fork(curthread->t_name,
                      child_proc,
                      call_enter_forked_process,
                      (void *)child_tf_copy,
                      0);

    if (err != 0) {
        kfree(child_tf_copy);
        proc_destroy(child_proc);
        return err;
    }

    *retval = child_proc->p_pid;
    return 0;
}

// Replaces current process image with a new executable.
// Copies program name, argv, loads ELF, builds new stack and jumps to user mode.
int
sys_execv(const char *progname, char *argv[])
{
    if (progname == NULL || argv == NULL) {
        return EFAULT;
    }
    if (curproc == NULL) {
        return ESRCH;
    }

    userptr_t uprog = (userptr_t)progname;
    userptr_t user_argv = (userptr_t)argv;

    int err;
    int argc;
    vaddr_t entrypoint;
    vaddr_t stackptr;

    char *kernel_path = kmalloc(PATH_MAX);
    if (kernel_path == NULL) {
        return ENOMEM;
    }

    err = copyinstr(uprog, kernel_path, PATH_MAX, NULL);
    if (err != 0) {
        kfree(kernel_path);
        return err;
    }

    arg_list_t kernel_args;
    arg_list_init(&kernel_args);

    err = arg_list_load_from_user(&kernel_args, user_argv);
    if (err != 0) {
        arg_list_free(&kernel_args);
        kfree(kernel_path);
        return err;
    }

    err = exec_loader(kernel_path, &entrypoint, &stackptr);
    if (err != 0) {
        arg_list_free(&kernel_args);
        kfree(kernel_path);
        return err;
    }

    kfree(kernel_path);

    err = arg_list_copy_to_userstack(&kernel_args, &stackptr, &argc, &user_argv);
    if (err != 0) {
        panic("execv: arg_list_copy_to_userstack failed");
    }

    arg_list_free(&kernel_args);

    enter_new_process(argc,
                      user_argv,
                      NULL,
                      stackptr,
                      entrypoint);

    panic("enter_new_process returned unexpectedly");
    return EINVAL;
}
