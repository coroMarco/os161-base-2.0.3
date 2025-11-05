/*
 * AUthor: G.Cabodi
 * Very simple implementation of sys_read and sys_write.
 * just works (partially) on stdin/stdout
 */

#include <types.h>
#include <kern/types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <limits.h>
#include <kern/errno.h>
#include <kern/stat.h>
#include <current.h>
#include <proc.h>
#include <fcntl.h>

struct openfile fileTable[OPEN_MAX];

static void trouble(int fd){
  if(curproc->fileTable[fd]==NULL){
    return;
  }

  if(curproc->fileTable[fd]->vn != NULL){
    vfs_close(curproc->fileTable[fd]->vn);
  }

  kfree(curproc->fileTable[fd]);
  curproc->fileTable[fd]=NULL;
}

int sys_open(userptr_t path, int openflags, mode_t mode, int *errp){

  size_t len;
  int fd=0;

  if(path == NULL || path > PATH_MAX){
    return EFAULT;
  }

  char *kbuffer = (char*) kmalloc(PATH_MAX*sizeof(char));
  
  if(kbuffer == NULL) return ENOMEM;

  int err = copyinstr((const_userptr_t) path,kbuffer,PATH_MAX,&len);
  if(err){
    kfree(kbuffer);
    return EFAULT;
  }

  struct vnode *v;
  err = vfs_open(kbuffer,openflags,mode,&v);
  if(err){
    kfree(kbuffer);
    return err;
  }
  kfree(kbuffer);

  struct openfile *of = NULL;
  for(int index = 0; index<OPEN_MAX;index++){
    
    if(fileTable[index].vn == NULL)
      of = &fileTable[index];
      of->vn=v;
      break;
  }

  if (of == NULL) return ENFILE;

  for(fd = 3; fd<OPEN_MAX;fd++){
    if(curproc->fileTable[fd] == NULL){
      curproc->fileTable[fd]=of;
      break;
    }

  }

  if (fd == OPEN_MAX-1) return EMFILE;

  if(openflags & O_APPEND){ //filtra i bit utili

    struct stat filestat;
    err = VOP_STAT(curproc->fileTable[fd]->vn,&filestat);
    if(err){
      kfree(curproc->fileTable[fd]);
      curproc->fileTable[fd]=NULL;
      return err;
    }
    curproc->fileTable[fd]->offset=filestat.st_size;
  }else{
    curproc->fileTable[fd]->offset = 0;
  }

  switch(openflags & O_ACCMODE){
    case O_RDONLY:
      curproc->fileTable[fd]->flags=O_RDONLY;
      break;
    case O_WRONLY:
          curproc->fileTable[fd]->flags=O_WRONLY;
          break;
    case O_RDWR:
          curproc->fileTable[fd]->flags=O_RDWR;
          break;
    default:
      trouble(fd);
      return EINVAL;
  }

  curproc->fileTable[fd]->lock=lock_create();
  if(curproc->fileTable[fd]->lock==NULL){
      trouble(fd);
      return ENOMEM;
  }

  *errp=fd;
  return 0;
}

/*
 * simple file system calls for write/read
 */
int sys_write(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
    kprintf("sys_write supported only to stdout\n");
    return -1;
  }

  for (i=0; i<(int)size; i++) {
    putch(p[i]);
  }

  return (int)size;
}

int
sys_read(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd!=STDIN_FILENO) {
    kprintf("sys_read supported only to stdin\n");
    return -1;
  }

  for (i=0; i<(int)size; i++) {
    p[i] = getch();
    if (p[i] < 0) 
      return i;
  }

  return (int)size;
}