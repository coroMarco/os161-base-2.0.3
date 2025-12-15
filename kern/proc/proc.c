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
	if (pid <= 0 || pid > PROC_MAX) {
		return NULL;
	}

	struct proc *proc = processTable.proc[pid];
	if (proc->p_pid != pid) {
		return NULL;
	}

	return proc;
}

/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
static void proc_init_waitpid(struct proc *proc, const char *name) {
#if OPT_WAITPID
  /* search a free index in table using a circular strategy */
  int i;
  spinlock_acquire(&processTable.lk);
  i = processTable.last_i+1;
  proc->p_pid = 0;
  if (i>MAX_PROC) i=1;
  while (i!=processTable.last_i) {
    if (processTable.proc[i] == NULL) {
      processTable.proc[i] = proc;
      processTable.last_i = i;
      proc->p_pid = i;
      break;
    }
    i++;
    if (i>MAX_PROC) i=1;
  }
  spinlock_release(&processTable.lk);
  if (proc->p_pid==0) {
    panic("too many processes. proc table is full\n");
  }
  proc->p_status = 0;
#if USE_SEMAPHORE_FOR_WAITPID
  proc->p_sem = sem_create(name, 0);
#else
  proc->p_cv = cv_create(name);
  proc->p_lock = lock_create(name);
#endif
#else
  (void)proc;
  (void)name;
#endif
}

/*
 * G.Cabodi - 2019
 * Terminate support for pid/waitpid.
 */
static void proc_end_waitpid(struct proc *proc) {
#if OPT_WAITPID
  /* remove the process from the table */
  int i;
  spinlock_acquire(&processTable.lk);
  i = proc->p_pid;
  KASSERT(i>0 && i<=MAX_PROC);
  processTable.proc[i] = NULL;
  spinlock_release(&processTable.lk);

#if USE_SEMAPHORE_FOR_WAITPID
  sem_destroy(proc->p_sem);
#else
  cv_destroy(proc->p_cv);
  lock_destroy(proc->p_lock);
#endif
#else
  (void)proc;
#endif
}

/*
 * Create a proc structure.
 */
static struct proc *proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	proc_init_waitpid(proc,name);

    bzero(proc->fileTable,OPEN_MAX*sizeof(struct openfile *));

	if(proc_setup(proc,name)<=0){
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}

	return proc;
}


static int proc_cleanup(struct proc *proc)
{
	spinlock_acquire(&processTable.lk);

	int index=proc->p_pid;
	if(index<0 || index>MAX_PROC || processTable.proc[index]!=proc) {
		return -1;
	}

	processTable.proc[index]=NULL;

	cv_destroy(proc->p_cv);
	lock_destroy(proc->p_lock);
	spinlock_release(&processTable.lk);	

	if(destroy_child_list(proc)==-1) return -1;
	
	if(proc->p_parent!=-1){
		parent_proc=proc_search_pid(proc->p_parent);
		if(proc->p_parent==kproc->p_pid)	parent_proc=kproc;
		if(parent_proc==NULL) return -1;

		if(destroy_child_from_list(parent_proc,proc->p_pid)==-1) return -1;

		return 0;
	}
}
/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	if(proc_cleanup(proc) != 0) {
		panic("proc_destroy: proc_cleanup failed\n");
	}

	proc_end_waitpid(proc);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
#if OPT_WAITPID
	spinlock_init(&processTable.lk);
	/* kernel process is not registered in the table */
	processTable.active = 1;
#endif
}


/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

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

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

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

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

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

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
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

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}



        /* G.Cabodi - 2019 - support for waitpid */
int  proc_wait(struct proc *proc){
#if OPT_WAITPID
        int return_status;
        /* NULL and kernel proc forbidden */
	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

        /* wait on semaphore or condition variable */ 
#if USE_SEMAPHORE_FOR_WAITPID
        P(proc->p_sem);
#else
        lock_acquire(proc->p_lock);
        cv_wait(proc->p_cv);
        lock_release(proc->p_lock);
#endif
        return_status = proc->p_status;
        proc_destroy(proc);
        return return_status;
#else
        /* this doesn't synchronize */ 
        (void)proc;
        return 0;
#endif
}


/* G.Cabodi - 2019 - support for waitpid */
void proc_signal_end(struct proc *proc)
{
#if USE_SEMAPHORE_FOR_WAITPID
      V(proc->p_sem);
#else
      lock_acquire(proc->p_lock);
      cv_signal(proc->p_cv);
      lock_release(proc->p_lock);
#endif
}

void call_enter_forked_process(void *tfv,unsigned long dummy){
	struct trapframe *tf = (struct trapframe *)tfv;
	enter_forked_process(tf);
	panic("enter_forked_process returned UNEXPECTED\n");
}

//function return:
// -1 error
// index pid free
int pid_allocate(void){

	int index=-1;
	if(processTable.last_pid+1>PROC_MAX){
		index=processTable.last_pid = 1;
	}
	else{
		index=processTable.last_pid++;
	}

	while(index!=processTable.last_pid){
		if(processTable.proc[index]==NULL) break;
		
		index++;
		
		if(index>PROC_MAX) {
			index=1;
		}else{
			index;
		}

		return index;
	}

	if(index==processTable.last_pid) return -1;

	return index;
}


int prod_add(pid_t pid,struct proc *proc){}

void proc_unregister_pid(pid_t pid){
	spinlock_acquire(&processTable.lk);
	processTable.proc[pid] = NULL;
	spinlock_release(&processTable.lk);
}

int add_new_child(struct proc* proc,pid_t child_pid){}

int destroy_child_from_list(struct proc*proc,pid_t child_pic){
struct child_list* app=proc->children_list;
	struct child_list* prev_child=NULL;


	while(app!=NULL){
		if(app->child_pid==child_pid){
			if(prev_child==NULL)
				proc->children_list=app->next_child;
			else
				prev_child->next_child=app->next_child;
			kfree(app);
			return 0;
		}
		prev_child=app;
		app=app->next_child;
	}
	
	return -1;
}

int proc_is_child(struct proc*proc,pid_t child_pid){
	
	struct child_list *cur;

	if (proc == NULL) {
		return -1;
	}

	cur = proc->children_list;
	while (cur != NULL) {
		if (cur->child_pid == child_pid) {
			return 0;
		}
		cur = cur->next_child;
	}

	return -1;
}


int destroy_child_list(struct proc* proc){
	struct child_list* app=proc->children_list;
    struct proc* child_proc;


    while(app!=NULL){
        proc->children_list=app->next_child;

        /FINDING THE CHILD STRUCTURE/
        child_proc=proc_search(app->child_pid);
        if(child_proc==NULL)
            return -1;

        /SETTING THE PARENT PID AS -1/
        child_proc->parent_pid=-1;

        /REMOVING THE CHILD/
        app->next_child=NULL;
        kfree(app);

        app=proc->children_list;
    }

    return 0;
}

#if OPT_FILE
void  proc_file_table_copy(struct proc *psrc, struct proc *pdest) {
	while(app!=NULL){
		if(app->child_pid==child_pid){
			return 0;
		}
		app=app->next_child;
	}
	
	return -1;
}


