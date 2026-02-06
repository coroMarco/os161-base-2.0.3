/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <syscall.h>

#include <synch.h>
#include <kern/fcntl.h>
#include <vfs.h>


#define MAX_PROC 100
static struct _processTable {
  int is_active;           /* initial value 0 */
  struct proc *proc[MAX_PROC+1]; /* [0] not used. pids are >= 1 */
  int last_pid;           /* index of last allocated pid */
  struct spinlock lk;	/* Lock for this table */
} processTable;

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;


 /*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
struct proc *proc_search_pid(pid_t pid) {
	if (pid <= 0 || pid > MAX_PROC) {
		return NULL;
	}

	struct proc *proc = processTable.proc[pid];
	if (proc->p_pid != pid) {
		return NULL;
	}

	return proc;
}

int proc_is_child(struct proc* proc, pid_t child_pid)
{	
	struct child_list* app=proc->children_list;


	while(app!=NULL){
		if(app->child_pid==child_pid){
			return 0;
		}
		app=app->next_child;
	}
	
	return -1;
}

/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
static int
proc_setup(struct proc *proc, const char *name)
{
    spinlock_acquire(&processTable.lk);

    int start = processTable.last_pid + 1;
    if (start > MAX_PROC) start = 1;

    int pid = start;
    do {
        if (processTable.proc[pid] == NULL) {
            processTable.proc[pid] = proc;
            processTable.last_pid = pid;
            proc->p_pid = pid;
            break;
        }
        pid++;
        if (pid > MAX_PROC) pid = 1;
    } while (pid != start);

    spinlock_release(&processTable.lk);

    if (proc->p_pid <= 0) {
        return -1;
    }

    proc->parent_pid = -1;
    proc->children_list = NULL;
    proc->p_status = 0;
    proc->p_exited = false;

    proc->p_lock = lock_create(name);
    proc->p_cv   = cv_create(name);

    if (proc->p_lock == NULL || proc->p_cv == NULL) {
        return -1;
    }

    return proc->p_pid;
}

/*
 * Create a proc structure.
 */
static struct proc *
proc_create(const char *name)
{
    struct proc *proc = kmalloc(sizeof(*proc));
    if (proc == NULL) return NULL;

    proc->p_name = kstrdup(name);
    if (proc->p_name == NULL) {
        kfree(proc);
        return NULL;
    }

    proc->p_numthreads = 0;
    spinlock_init(&proc->p_spinlk);

    proc->p_addrespace = NULL;
    proc->p_cwd = NULL;
    bzero(proc->fileTable, OPEN_MAX * sizeof(struct openfile *));

    if (strcmp(name, "[kernel]") != 0) {
        if (proc_setup(proc, name) < 0) {
            kfree(proc->p_name);
            kfree(proc);
            return NULL;
        }
    }

    return proc;
}


int destroy_child_list(struct proc *proc)
{
    struct child_list *app = proc->children_list;
    struct child_list *next;

    while (app != NULL) {
        next = app->next_child;

        struct proc *child_proc = proc_search_pid(app->child_pid);
        if (child_proc != NULL) {
            child_proc->parent_pid = -1; // orfani
        }

        kfree(app);
        app = next;
    }

    proc->children_list = NULL;
    return 0;
}

static void
proc_cleanup(struct proc *proc)
{
    spinlock_acquire(&processTable.lk);
    processTable.proc[proc->p_pid] = NULL;
    spinlock_release(&processTable.lk);

    destroy_child_list(proc);

    if (proc->parent_pid != -1) {
        struct proc *parent = proc_search_pid(proc->parent_pid);
        if (parent != NULL) {
            destroy_child_from_list(parent, proc->p_pid);
        }
    }

    cv_destroy(proc->p_cv);
    lock_destroy(proc->p_lock);
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
    KASSERT(proc != NULL);
    KASSERT(proc != kproc);

    if (proc->p_cwd) {
        VOP_DECREF(proc->p_cwd);
    }

    if (proc->p_addrespace) {
        struct addrspace *as;
        if (proc == curproc) {
            as = proc_setas(NULL);
            as_deactivate();
        } else {
            as = proc->p_addrespace;
        }
        as_destroy(as);
    }

    KASSERT(proc->p_numthreads == 0);
    spinlock_cleanup(&proc->p_spinlk);

    proc_cleanup(proc);

    kfree(proc->p_name);
    kfree(proc);
}


/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
    spinlock_init(&processTable.lk);

    for (int i = 0; i <= MAX_PROC; i++) {
        processTable.proc[i] = NULL;
    }

    kproc = proc_create("[kernel]");
    if (kproc == NULL) {
        panic("proc_create for kproc failed\n");
    }

    /* PID 0 RISERVATO al kernel */
    kproc->p_pid = 0;
    processTable.proc[0] = kproc;
    processTable.last_pid = 0;
}



/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
static int console_init(const char *lock_name, struct proc *proc, int fd, int flag) {

	/* ASSIGNMENT OF THE CONSOLE NAME */
	char *con = kstrdup("con:");
	if (con == NULL) {
		return -1;
	}

	/* ALLOCATING SPACE IN THE FILETABLE */
	proc->fileTable[fd] = (struct openfile *) kmalloc(sizeof(struct openfile));
	if (proc->fileTable[fd] == NULL) {
		kfree(con);
		return -1;
	}

	/* OPENING ASSOCIATED FILE */
	int err = vfs_open(con, flag, 0644, &proc->fileTable[fd]->vn);
	if (err) {
		kfree(con);
		kfree(proc->fileTable[fd]);
		return -1;
	}
	kfree(con);

	/* INITIALIZATION OF VALUES */
	proc->fileTable[fd]->offset = 0;
	proc->fileTable[fd]->lock = lock_create(lock_name);
	if (proc->fileTable[fd]->lock == NULL) {
		vfs_close(proc->fileTable[fd]->vn);
		kfree(proc->fileTable[fd]);
		return -1;
	}
	proc->fileTable[fd]->counter_ref = 1;
	proc->fileTable[fd]->flags = flag;

	return 0;
}

struct proc *proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrespace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	/* CONSOLE INITIALIZATION FOR STDIN, STDOUT AND STDERR */
	if (console_init("STDIN", newproc, 0, O_RDONLY) == -1) {
		return NULL;
	} else if (console_init("STDOUT", newproc, 1, O_WRONLY) == -1) {
		return NULL;
	} else if (console_init("STDERR", newproc, 2, O_WRONLY) == -1) {
		return NULL;
	}

	spinlock_acquire(&curproc->p_spinlk);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_spinlk);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_spinlk);
	proc->p_numthreads++;
	spinlock_release(&proc->p_spinlk);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_spinlk);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_spinlk);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace * proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_spinlk);
	as = proc->p_addrespace;
	spinlock_release(&proc->p_spinlk);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace * proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_spinlk);
	oldas = proc->p_addrespace;
	proc->p_addrespace = newas;
	spinlock_release(&proc->p_spinlk);
	return oldas;
}



/*
 * G.Cabodi - 2019 - support for waitpid
 */
void call_enter_forked_process(void *tfv,unsigned long dummy){
	(void) dummy;
	struct trapframe *tf = (struct trapframe *)tfv;
	enter_forked_process(tf);
	panic("enter_forked_process returned UNEXPECTED\n");
}

//function return:
// -1 error
// index pid free
int pid_allocate(void)
{
    spinlock_acquire(&processTable.lk);

    int start = processTable.last_pid + 1;
    if (start > MAX_PROC) start = 1;

    int pid = start;
    do {
        if (processTable.proc[pid] == NULL) {
            processTable.last_pid = pid;
            spinlock_release(&processTable.lk);
            return pid;
        }
        pid++;
        if (pid > MAX_PROC) pid = 1;
    } while (pid != start);

    spinlock_release(&processTable.lk);
    return -1; // Nessun PID libero
}


// Add the given process to the process table, at the given index.
// 0 success, -1 error
int proc_register_pid(pid_t pid,struct proc *proc){

	if(pid<=0 || pid>MAX_PROC+1 || proc==NULL) return -1;

	spinlock_acquire(&processTable.lk);
		processTable.proc[pid]=proc;
		processTable.last_pid=pid;
	spinlock_release(&processTable.lk);

	return 0;
}

void proc_unregister_pid(pid_t pid){
	spinlock_acquire(&processTable.lk);
	processTable.proc[pid] = NULL;
	spinlock_release(&processTable.lk);
}

int add_new_child(struct proc *parent, pid_t child_pid)
{
	struct child_list* app=parent->children_list;


	/* If the list is empty, create the first element. */
	if(parent->children_list==NULL){
		parent->children_list=(struct child_list *) kmalloc(sizeof(struct child_list));
		if(parent->children_list==NULL)
			return -1;
		parent->children_list->next_child=NULL;
		parent->children_list->child_pid=child_pid;
		return 0;
	}

	/* Otherwise, append a new node at the end of the list. */
	while(app->next_child!=NULL){
		app=app->next_child;
	}

	app->next_child=(struct child_list *) kmalloc(sizeof(struct child_list));
	if(app->next_child==NULL)
		return -1;
	app->next_child->next_child=NULL;
	app->next_child->child_pid=child_pid;
	return 0;
}

int destroy_child_from_list(struct proc *proc, pid_t child_pid)
{
    struct child_list *app = proc->children_list;
    struct child_list *prev = NULL;

    while (app != NULL) {
        if (app->child_pid == child_pid) {
            if (prev == NULL) {
                proc->children_list = app->next_child;
            } else {
                prev->next_child = app->next_child;
            }
            kfree(app);
            return 0;
        }
        prev = app;
        app = app->next_child;
    }

    return -1; // Child non trovato
}


