// exec_support.h
// Support structures and prototypes used by execv.
// Handles argument buffers and executable loading routines.

#ifndef _EXEC_SUPPORT_H_
#define _EXEC_SUPPORT_H_

#include <types.h>
#include <synch.h>

// Argument buffer descriptor used for handling argv copying.
typedef struct arg_list {
    char *buffer;           // Storage area for argument strings
    size_t used_bytes;      // Bytes currently used in buffer
    size_t capacity;        // Total allocated size of buffer
    int argc;               // Number of stored arguments
    bool has_large_arg_sem; // True if exec_large_arg_sem has been acquired
} arg_list_t;

// Semaphore used to throttle large ARG_MAX allocations.
extern struct semaphore *exec_large_arg_sem;

// Maximum number of concurrent large-argv allocations allowed.
#define EXEC_LARGE_ARG_LIMIT 1

// Initializes the exec system (allocates exec_large_arg_sem).
void exec_init(void);

// Initializes an arg_list descriptor (no allocation).
void arg_list_init(arg_list_t *args);

// Allocates storage for arg_list data.
int arg_list_alloc(arg_list_t *args, size_t size);

// Copies argv from user space into the kernel arg_list.
int arg_list_copy_from_user(arg_list_t *args, userptr_t user_argv);

// Copies argument strings and argv[] layout onto the user stack.
int arg_list_copy_to_userstack(arg_list_t *args, vaddr_t *user_stack_ptr,
                               int *argc_ret, userptr_t *uargv_ret);

// Wrapper that copies argv from userland using PAGE_SIZE first,
// then ARG_MAX + throttling if needed.
int arg_list_load_from_user(arg_list_t *args, userptr_t user_argv);

// Frees arg_list storage and releases semaphore if needed.
void arg_list_free(arg_list_t *args);

// Loads an executable into a new address space.
int exec_loader(char *path, vaddr_t *entrypoint, vaddr_t *stackptr);

#endif
