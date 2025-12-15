/*
 * AUthor: G.Cabodi
 * Very simple implementation of sys_read and sys_write.
 * just works (partially) on stdin/stdout
 */


#include <syscall.h>
#include <types.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <vfs.h>
#include <uio.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/stat.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/unistd.h>
#include <endian.h>
#include <stat.h>
#include <lib.h>
#include <kern/seek.h>

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

  if(path==NULL) return EFAULT;

  size_t file_len;
  int fd=0,err;
  char *kfilename;
  struct vnode *v;
  struct openfile *of = NULL;
  file_len=strlen((char*)path);

  kfilename = (char *) kmalloc(PATH_MAX * sizeof(char));
  
  if(kfilename == NULL) return ENOMEM;


  err = copyinstr((const_userptr_t) path,kfilename,PATH_MAX,&file_len);
  if(err){
    kfree(kfilename);
    return EFAULT;
  }

  
  err = vfs_open(kfilename,openflags,mode,&v); //already handling O_CREATE
  if(err){
    kfree(kfilename);
    return err;
  }
  kfree(kfilename);

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

  // PLUS: handle O_APPEND 
  if(openflags & O_APPEND){ //x & y filtra i bit utili

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

  curproc->fileTable[fd]->lock=lock_create("lock file");
  if(curproc->fileTable[fd]->lock==NULL){
      trouble(fd);
      return ENOMEM;
  }

  *errp=fd;
  return 0;
}


// sys_write
// Write up to `buflen` bytes from user buffer `buf` to the file referred by `fd`,
// starting at the current file offset. The file must be open for writing.
// On success, *retval is set to the number of bytes actually written and 0 is returned.
// On error, an appropriate errno value is returned (e.g., EBADF, EFAULT, ENOMEM, EIO).
ssize_t sys_write(int fd, const void *buf, size_t buflen, int32_t *retval)
{
    struct openfile *ofile;
    struct vnode    *vnode;
    struct iovec     iov;
    struct uio       kuio;
    char            *kbuf;
    off_t            old_offset;
    int              err;

    // validate file descriptor range
    if (fd < 0 || fd >= OPEN_MAX) {
        return EBADF;
    }

    // retrieve open file entry
    ofile = curproc->fileTable[fd];
    if (ofile == NULL) {
        return EBADF;
    }

    // file must be open for writing
    if (ofile->flags == O_RDONLY) {
        return EBADF;
    }

    // validate user buffer pointer
    if (buf == NULL) {
        return EFAULT;
    }

    // nothing to write
    if (buflen == 0) {
        *retval = 0;
        return 0;
    }

    // allocate kernel buffer
    kbuf = kmalloc(buflen);
    if (kbuf == NULL) {
        return ENOMEM;
    }

    // copy data from user space to kernel space
    err = copyin((const_userptr_t)buf, kbuf, buflen);
    if (err) {
        kfree(kbuf);
        return EFAULT;
    }

    // perform write operation
    lock_acquire(ofile->lock);

    vnode      = ofile->vn;
    old_offset = ofile->offset;

    uio_kinit(&iov, &kuio, kbuf, buflen, old_offset, UIO_WRITE);
    err = VOP_WRITE(vnode, &kuio);
    if (err) {
        lock_release(ofile->lock);
        kfree(kbuf);
        return err;
    }

    // update file offset and report bytes written
    ofile->offset = kuio.uio_offset;
    *retval       = (int32_t)(kuio.uio_offset - old_offset);

    lock_release(ofile->lock);
    kfree(kbuf);

    return 0;
}

// sys_read
// Read up to `buflen` bytes from the file referred by `fd` into user buffer `buf`,
// starting at the current file offset. The file must be open for reading.
// On success, *retval is set to the number of bytes actually read and 0 is returned.
// On error, an appropriate errno value is returned (e.g., EBADF, EFAULT, ENOMEM, EIO).
ssize_t sys_read(int fd, const void *buf, size_t buflen, int32_t *retval)
{
    struct openfile *ofile;
    struct vnode    *vnode;
    struct iovec     iov;
    struct uio       kuio;
    char            *kbuf;
    size_t           nread;
    int              err;

    // validate file descriptor range
    if (fd < 0 || fd >= OPEN_MAX) {
        return EBADF;
    }

    // retrieve open file entry
    ofile = curproc->fileTable[fd];
    if (ofile == NULL) {
        return EBADF;
    }

    // file must be open for reading
    if (ofile->flags == O_WRONLY) {
        return EBADF;
    }

    // validate user buffer pointer
    if (buf == NULL) {
        return EFAULT;
    }

    // nothing to read
    if (buflen == 0) {
        *retval = 0;
        return 0;
    }

    // allocate kernel buffer
    kbuf = kmalloc(buflen);
    if (kbuf == NULL) {
        return ENOMEM;
    }

    vnode = ofile->vn;

    // perform read operation
    lock_acquire(ofile->lock);

    uio_kinit(&iov, &kuio, kbuf, buflen, ofile->offset, UIO_READ);
    err = VOP_READ(vnode, &kuio);
    if (err) {
        lock_release(ofile->lock);
        kfree(kbuf);
        return err;
    }

    // update file offset and compute bytes read
    ofile->offset = kuio.uio_offset;
    nread         = buflen - kuio.uio_resid;
    *retval       = (int32_t)nread;

    lock_release(ofile->lock);

    // copy data from kernel buffer to user buffer
    err = copyout(kbuf, (userptr_t)buf, nread);
    if (err) {
        kfree(kbuf);
        return EFAULT;
    }

    kfree(kbuf);
    return 0;
}

int sys_close(int fd){
  
      if (fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
        return EBADF;
      }

      struct openfile *of= curproc->fileTable[fd];
      lock_acquire(of->lock);

      curproc->fileTable[fd]=NULL;
      --of->counter_ref;
      
      if(of->counter_ref>0){
        lock_release(of->lock);
        return 0;
      }else{
        struct vnode *vn = of->vn;
        of->vn=NULL;      
        vfs_close(vn);
      }
      
      lock_release(of->lock);
      kfree(of);
      return 0;
    
}

int sys_remove(const char* pathname){
  
  (void)pathname;
    return 0;
}

int sys_chdir(const char* pathname){
  
  KASSERT(curthread!=NULL);
  KASSERT(curthread->t_proc!=NULL);


  char *kpath;
  int result;

  if(pathname==NULL) return EFAULT;

  kpath = (char*)kmalloc(PATH_MAX*sizeof(char));
  if(kpath == NULL) return ENOMEM;

  result = copyinstr((const_userptr_t)pathname,kpath,PATH_MAX,NULL);
  if(result){
    kfree(kpath);
    return result;
  }

  //open dir pointed by path
  result = vfs_chdir(kpath);
  kfree(kpath);

  return result;
}

int sys_getcwd(const char *buf,size_t buflen,int *retval){

  struct uio uio;
    struct iovec iov;
    int result;
    size_t len;

    // validate user buffer pointer
    if (buf == NULL) {
        return EFAULT;
    }

    // buffer length must be non-zero
    if (buflen == 0) {
        return EINVAL;
    }

    // current working directory must exist
    if (curproc->p_cwd == NULL) {
        return ENOENT;
    }

    // initialize uio to write directly into user space
    iov.iov_ubase = (userptr_t)buf;
    iov.iov_len   = buflen;

    uio.uio_iov     = &iov;
    uio.uio_iovcnt  = 1;
    uio.uio_resid   = buflen;
    uio.uio_offset  = 0;
    uio.uio_segflg  = UIO_USERSPACE;
    uio.uio_rw      = UIO_READ;
    uio.uio_space   = proc_getas();

    // retrieve absolute path of the current working directory
    result = vfs_getcwd(&uio);
    if (result) {
        return result;
    }

    // compute number of bytes written (including null terminator)
    len = buflen - uio.uio_resid;
    *retval = (int)len;

    return 0;

}

off_t sys_lseek(int fd,off_t pos,int whence,int *retval){

      KASSERT(curproc != NULL);

      if (fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
        return EBADF;
      }

      if(!VOP_ISSEEKABLE(curproc->fileTable[fd]->vn)){
        return ESPIPE;
      }

      struct openfile *of= curproc->fileTable[fd];

      lock_acquire(of->lock);
      
      int newpos=0;
      int err;
      struct stat info;
      
      
      switch(whence){
        case SEEK_SET:

              newpos=pos;
              
              if(pos<0) {
                lock_release(of->lock);
                return EINVAL;
              }
              
              of->offset=pos;

            break;
        case SEEK_CUR:
            
              newpos=of->offset+pos;
              
              if(newpos<0) {
                lock_release(of->lock);
                return EINVAL;
              }

              of->offset=newpos;
            
            break;
        case SEEK_END:

            err = VOP_STAT(of->vn,&info);
            if(err){
              lock_release(of->lock);
              return err;
            }

            if(pos<0 && -pos>info.st_size){
              lock_release(of->lock);
              return EINVAL;
            }
            of->offset=info.st_size-pos;
          break;
        default:
            lock_release(of->lock);
            return EINVAL;
          break;     
      }

      lock_release(of->lock);

      *retval=newpos;
      return 0;
      
}

int sys_dup2(int oldfd,int newfd,int *retval){

      struct openfile *of;

      KASSERT(curproc != NULL);

      if(oldfd<0 || oldfd >= OPEN_MAX || newfd<0 || newfd >= OPEN_MAX){
        return EBADF;
      }

      if(curproc->fileTable[oldfd] == NULL){
        return EBADF;
      }

      if(oldfd == newfd){
        *retval=newfd;
        return 0;
      }

      if(curproc->fileTable[newfd] != NULL){
        sys_close(newfd);
        of=NULL;
      } 

      of = curproc->fileTable[oldfd];
      lock_acquire(of->lock);
      of->counter_ref++;
      lock_release(of->lock);
      curproc->fileTable[newfd]=of;

      *retval=newfd;
      return 0;
}