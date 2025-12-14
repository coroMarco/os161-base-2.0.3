// Support code for the execv() system call.
// Handles argv buffers and common executable loading routine.

#include <types.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <vfs.h>
#include <uio.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/unistd.h>
#include <endian.h>
#include <stat.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <kern/wait.h>
#include <mips/trapframe.h>
#include <syscall.h>
#include "exec_support.h"

// Initialize exec-related global state.
// Creates the semaphore used to throttle large argv allocations.
// Panics if the semaphore cannot be created.
void
exec_init(void)
{
    if (exec_large_arg_sem != NULL) {
        return;
    }

    exec_large_arg_sem = sem_create("exec_large_arg_sem", EXEC_LARGE_ARG_LIMIT);
    if (exec_large_arg_sem == NULL) {
        panic("exec_init: cannot create exec_large_arg_sem\n");
    }
}

// Initialize an argument buffer descriptor.
// Resets all fields to a clean state.
// Does not allocate backing storage.
void
arg_list_init(arg_list_t *args)
{
    if (args == NULL) {
        return;
    }

    args->buffer            = NULL;
    args->used_bytes        = 0;
    args->capacity          = 0;
    args->argc              = 0;
    args->has_large_arg_sem = false;
}

// Allocate backing storage for an argument buffer.
// size is the number of bytes requested for the strings area.
// Returns 0 on success or ENOMEM / EINVAL on failure.
int
arg_list_alloc(arg_list_t *args, size_t size)
{
    if (args == NULL) {
        return EINVAL;
    }

    if (size == 0) {
        return EINVAL;
    }

    args->buffer = (char *)kmalloc(size);
    if (args->buffer == NULL) {
        args->capacity   = 0;
        args->used_bytes = 0;
        args->argc       = 0;
        return ENOMEM;
    }

    args->capacity   = size;
    args->used_bytes = 0;
    args->argc       = 0;

    return 0;
}

// Release resources associated with an argument buffer.
// Frees backing storage and releases the exec semaphore if taken.
// After this call the buffer is back to an empty state.
void
arg_list_free(arg_list_t *args)
{
    if (args == NULL) {
        return;
    }

    if (args->buffer != NULL) {
        kfree(args->buffer);
        args->buffer = NULL;
    }

    args->used_bytes = 0;
    args->capacity   = 0;
    args->argc       = 0;

    if (args->has_large_arg_sem) {
        V(exec_large_arg_sem);
        args->has_large_arg_sem = false;
    }
}

// Copy argv from user space into the kernel buffer.
// Expects args->buffer to be allocated and args->capacity > 0.
// On success fills args->used_bytes (bytes used) and args->argc (argument count).
// Returns 0 on success, or an errno value on failure (E2BIG if too large).
int
arg_list_copy_from_user(arg_list_t *args, userptr_t user_argv)
{
    userptr_t arg_ptr;
    size_t arg_len;
    int result;

    if (args == NULL || args->buffer == NULL || args->capacity == 0) {
        return EINVAL;
    }

    if (user_argv == NULL) {
        return EFAULT;
    }

    args->used_bytes = 0;
    args->argc       = 0;

    while (1) {
        result = copyin(user_argv, &arg_ptr, sizeof(userptr_t));
        if (result != 0) {
            return result;
        }

        if (arg_ptr == NULL) {
            break;
        }

        if (args->used_bytes >= args->capacity) {
            return E2BIG;
        }

        result = copyinstr(arg_ptr,
                           args->buffer + args->used_bytes,
                           args->capacity - args->used_bytes,
                           &arg_len);
        if (result == ENAMETOOLONG) {
            return E2BIG;
        }

        if (result != 0) {
            return result;
        }

        args->used_bytes += arg_len;
        args->argc       += 1;
        user_argv        += sizeof(userptr_t);
    }

    return 0;
}

// Copy argument strings and argv[] layout onto the user stack.
// Uses the current *user_stack_ptr as the top of the stack and moves it down.
// On success updates *user_stack_ptr, writes argc, and returns user argv pointer.
// Returns 0 on success or an errno value on failure.
int
arg_list_copy_to_userstack(arg_list_t *args, vaddr_t *user_stack_ptr,
                           int *argc_ret, userptr_t *uargv_ret)
{
    vaddr_t user_stack_top;
    userptr_t user_str_base;
    userptr_t user_argv_base;
    userptr_t uargv_i;
    userptr_t arg_ptr;
    size_t arg_len;
    size_t offset;
    int result;

    if (args == NULL || user_stack_ptr == NULL || argc_ret == NULL || uargv_ret == NULL) {
        return EINVAL;
    }

    if (args->buffer == NULL || args->used_bytes > args->capacity) {
        return EINVAL;
    }

    user_stack_top = *user_stack_ptr;

    // Reserve space for argument strings.
    user_stack_top -= args->used_bytes;
    user_stack_top -= (user_stack_top & (sizeof(void *) - 1));
    user_str_base = (userptr_t)user_stack_top;

    // Reserve space for argv pointers (argc + terminating NULL).
    user_stack_top -= (args->argc + 1) * sizeof(userptr_t);
    user_argv_base = (userptr_t)user_stack_top;

    offset  = 0;
    uargv_i = user_argv_base;

    while (offset < args->used_bytes) {
        arg_ptr = user_str_base + offset;

        result = copyout(&arg_ptr, uargv_i, sizeof(arg_ptr));
        if (result != 0) {
            return result;
        }

        result = copyoutstr(args->buffer + offset,
                            arg_ptr,
                            args->used_bytes - offset,
                            &arg_len);
        if (result != 0) {
            return result;
        }

        offset  += arg_len;
        uargv_i += sizeof(arg_ptr);
    }

    KASSERT(offset == args->used_bytes);

    arg_ptr = NULL;
    result = copyout(&arg_ptr, uargv_i, sizeof(userptr_t));
    if (result != 0) {
        return result;
    }

    *user_stack_ptr = user_stack_top;
    *argc_ret       = args->argc;
    *uargv_ret      = user_argv_base;

    return 0;
}

// Copy argv from user space into a kernel arg_list.
// First tries a small PAGE_SIZE buffer; if too small (E2BIG) retries with ARG_MAX.
// The second attempt uses exec_large_arg_sem to limit large allocations.
// On success returns 0; on error returns errno. The caller must always call
// arg_list_free() after arg_list_load_from_user(), even on failure.
int
arg_list_load_from_user(arg_list_t *args, userptr_t user_argv)
{
    int result;

    if (args == NULL) {
        return EINVAL;
    }

    arg_list_init(args);

    result = arg_list_alloc(args, PAGE_SIZE);
    if (result != 0) {
        return result;
    }

    result = arg_list_copy_from_user(args, user_argv);
    if (result != E2BIG) {
        return result;
    }

    arg_list_free(args);
    arg_list_init(args);

    P(exec_large_arg_sem);
    args->has_large_arg_sem = true;

    result = arg_list_alloc(args, ARG_MAX);
    if (result != 0) {
        return result;
    }

    result = arg_list_copy_from_user(args, user_argv);
    return result;
}

// Load an executable into a new address space.
// Opens the file, creates and installs a new address space, loads the ELF,
// defines the user stack, destroys the old address space, and renames the thread.
// On success writes entrypoint and stackptr and returns 0.
// On failure leaves the old address space active and returns errno.
int
exec_loader(char *path, vaddr_t *entrypoint, vaddr_t *stackptr)
{
    struct addrspace *newas;
    struct addrspace *oldas;
    struct vnode *vn;
    char *newname;
    int err;

    if (path == NULL || entrypoint == NULL || stackptr == NULL) {
        return EINVAL;
    }

    newname = kstrdup(path);
    if (newname == NULL) {
        return ENOMEM;
    }

    err = vfs_open(path, O_RDONLY, 0, &vn);
    if (err != 0) {
        kfree(newname);
        return err;
    }

    newas = as_create();
    if (newas == NULL) {
        vfs_close(vn);
        kfree(newname);
        return ENOMEM;
    }

    oldas = proc_setas(newas);
    as_activate();

    err = load_elf(vn, entrypoint);
    if (err != 0) {
        vfs_close(vn);
        proc_setas(oldas);
        as_activate();
        as_destroy(newas);
        kfree(newname);
        return err;
    }

    vfs_close(vn);

    err = as_define_stack(newas, stackptr);
    if (err != 0) {
        proc_setas(oldas);
        as_activate();
        as_destroy(newas);
        kfree(newname);
        return err;
    }

    if (oldas != NULL) {
        as_destroy(oldas);
    }

    kfree(curthread->t_name);
    curthread->t_name = newname;

    return 0;
}
