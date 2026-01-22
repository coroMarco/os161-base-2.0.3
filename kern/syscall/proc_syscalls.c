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
#include "syscall.h"
#include "exec_support.h"

// Returns the pid of the current process in *retval.
// Never fails once curproc is valid.
//TODO
/*int
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
}*/

int sys_getpid(pid_t *retval) {

    /* RETRIEVING PID */
    KASSERT(curproc != NULL);
    *retval = curproc->p_pid;

    /* getpid() DOES NOT FAIL.  */
    return 0;   
}

// Waits for a specific child pid and retrieves its exit status.
// On success stores return status in *child_status and pid in *retval.
int sys_waitpid(pid_t pid, int *status, int options, int32_t *retval)
{
    if (pid <= 0) return ESRCH;
    if (curproc == NULL) return ESRCH;

    /* Check child relationship */
    if (proc_is_child(curproc, pid) != 0) {
        return ECHILD;
    }

    struct proc *child = proc_search_pid(pid);
    if (child == NULL) return ESRCH;

    if (options != 0 && options != WNOHANG) return EINVAL;

    lock_acquire(child->p_lock);

    if (options == WNOHANG) {
        if (!child->p_exited) {
            *retval = 0;
            lock_release(child->p_lock);
            return 0;
        }
    } else {
        while (!child->p_exited) {
            cv_wait(child->p_cv, child->p_lock);
        }
    }

    /* Copy exit status safely to user memory */
    if (status != NULL) {
        int err = copyout(&child->p_status, (userptr_t)status, sizeof(int));
        if (err) {
            lock_release(child->p_lock);
            return err; // EFAULT
        }
    }

    *retval = pid;

    lock_release(child->p_lock);

   /* Remove child from parent's list */
    destroy_child_from_list(curproc, pid);

    /* Mark child as reaped */
    child->parent_pid = -1;

    /* Now it is safe to destroy */
    proc_destroy(child);


    return 0;
}

// Terminates the current process and notifies parent waiters.
// Does not return.
void sys__exit(int exitcode)
{
    struct proc *p = curproc;
    KASSERT(p != NULL);

    /* Set exit status and mark as exited */
    p->p_status = _MKWAIT_EXIT(exitcode);
    p->p_exited = true;

    /* Remove this thread from the process */
    proc_remthread(curthread);

    /* Wake up any waiters */
    lock_acquire(p->p_lock);
    cv_broadcast(p->p_cv, p->p_lock);
    lock_release(p->p_lock);

    /* Detach children (orphan them) */
    struct child_list *child = p->children_list;
    while (child != NULL) {
        struct proc *child_proc = proc_search_pid(child->child_pid);
        if (child_proc != NULL) {
            child_proc->parent_pid = -1;
        }
        child = child->next_child;
    }
    /* Non distruggere i figli orfani qui! */

    /* Thread terminates here */
    thread_exit();

    panic("sys_exit: thread_exit returned unexpectedly\n");
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

    int err = as_copy(curproc->p_addrespace, &child_proc->p_addrespace);
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
        //TODO
        //kfree(child_tf_copy);
        proc_destroy(child_proc);
        return ENOMEM;
    }

    child_proc->parent_pid = curproc->p_pid;

    err = proc_register_pid((pid_t)new_pid, child_proc);
    if (err == -1) {
        //TODO
        //kfree(child_tf_copy);
        //proc_destroy(child_proc);
        return ENOMEM;
    }

    err = thread_fork(curthread->t_name,
                      child_proc,
                      call_enter_forked_process,
                      (void *)child_tf_copy,
                      (unsigned long)0);

    if (err) {
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
