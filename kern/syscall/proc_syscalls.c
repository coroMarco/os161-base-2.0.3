/*
 * AUthor: G.Cabodi
 * Very simple implementation of sys__exit.
 * It just avoids crash/panic. Full process exit still TODO
 * Address space is released
 */

#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <current.h>
#include <synch.h>

/*
 * system calls for process management
 */
void
sys__exit(int status){}

int sys_waitpid(pid_t pid, userptr_t statusp, int options){}

int sys_getpid(pid_t *retpid){

    KASSER(curproc!=NULL);
    *retpid=curproc->p_pid;

    return 0;
}

int sys_getppid(pid_t *retpid){
    *retpid = (int) curproc->parent_pid;
    return 0;
}


int sys_fork(struct trapframe *ctf, pid_t *retval) {}

int sys_execv(const char *progname,char *argv[]);
