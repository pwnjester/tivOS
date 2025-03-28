#include <disk.h>
#include <ext2.h>
#include <fat32.h>
#include <linked_list.h>
#include <malloc.h>
#include <poll.h>
#include <string.h>
#include <syscalls.h>
#include <system.h>
#include <task.h>
#include <unixSocket.h>
#include <util.h>
#include <vfs.h>

// Simple VFS abstraction to manage filesystems

OpenFile *fsRegisterNode(Task *task) {
  TaskInfoFiles *files = task->infoFiles;
  spinlockCntWriteAcquire(&files->WLOCK_FILES);
  OpenFile *ret =
      LinkedListAllocate((void **)&files->firstFile, sizeof(OpenFile));
  spinlockCntWriteRelease(&files->WLOCK_FILES);
  return ret;
}

bool fsUnregisterNode(Task *task, OpenFile *file) {
  TaskInfoFiles *files = task->infoFiles;
  spinlockCntWriteAcquire(&files->WLOCK_FILES);
  bool ret = LinkedListUnregister((void **)&files->firstFile, file);
  spinlockCntWriteRelease(&files->WLOCK_FILES);
  return ret;
}

// TODO! flags! modes!
// todo: openId in a bitmap or smth, per task/kernel
// note the dup2() syscall!

char     *prefix = "/";
int       openId = 3;
OpenFile *fsOpenGeneric(char *filename, Task *task, int flags, int mode) {
  spinlockAcquire(&task->infoFs->LOCK_FS);
  char *safeFilename = fsSanitize(task ? task->infoFs->cwd : prefix, filename);
  spinlockRelease(&task->infoFs->LOCK_FS);

  OpenFile *target = fsRegisterNode(task);
  target->id = openId++;
  target->mode = mode;
  target->flags = flags;

  target->pointer = 0;
  target->tmp1 = 0;

  MountPoint *mnt = fsDetermineMountPoint(safeFilename);
  if (!mnt) {
    // no mountpoint for this
    fsUnregisterNode(task, target);
    free(target);
    free(safeFilename);
    return 0;
  }
  target->mountPoint = mnt;
  target->handlers = mnt->handlers;

  if (flags & O_CLOEXEC)
    target->closeOnExec = true;

  char *strippedFilename = fsStripMountpoint(safeFilename, mnt);
  // check for open handler
  if (target->handlers->open) {
    char  *symlink = 0;
    size_t ret =
        target->handlers->open(strippedFilename, flags, mode, target, &symlink);
    if (RET_IS_ERR(ret)) {
      // failed to open
      fsUnregisterNode(task, target);
      free(target);
      free(safeFilename);

      if (symlink && ret != ERR(ELOOP)) {
        // we came across a symbolic link
        char *symlinkResolved = fsResolveSymlink(mnt, symlink);
        free(symlink);
        OpenFile *res = fsOpenGeneric(symlinkResolved, task, flags, mode);
        free(symlinkResolved);
        return res;
      }
      return (OpenFile *)((size_t)(ret));
    }
    free(safeFilename);
  }
  return target;
}

// returns an ORPHAN!
OpenFile *fsUserDuplicateNodeUnsafe(OpenFile *original) {
  OpenFile *orphan = (OpenFile *)malloc(sizeof(OpenFile));
  orphan->next = 0; // duh

  memcpy((void *)((size_t)orphan + sizeof(orphan->next)),
         (void *)((size_t)original + sizeof(original->next)),
         sizeof(OpenFile) - sizeof(orphan->next));

  if (original->handlers->duplicate &&
      !original->handlers->duplicate(original, orphan)) {
    free(orphan);
    return 0;
  }

  return orphan;
}

OpenFile *fsUserDuplicateNode(void *taskPtr, OpenFile *original) {
  Task          *task = (Task *)taskPtr;
  TaskInfoFiles *files = task->infoFiles;

  OpenFile *target = fsUserDuplicateNodeUnsafe(original);
  target->id = openId++;

  spinlockCntWriteAcquire(&files->WLOCK_FILES);
  LinkedListPushFrontUnsafe((void **)(&files->firstFile), target);
  spinlockCntWriteRelease(&files->WLOCK_FILES);

  return target;
}

OpenFile *fsUserGetNode(void *task, int fd) {
  Task          *target = (Task *)task;
  TaskInfoFiles *files = target->infoFiles;
  spinlockCntReadAcquire(&files->WLOCK_FILES);
  OpenFile *browse = files->firstFile;
  while (browse) {
    if (browse->id == fd)
      break;

    browse = browse->next;
  }
  spinlockCntReadRelease(&files->WLOCK_FILES);

  return browse;
}

OpenFile *fsKernelOpen(char *filename, int flags, uint32_t mode) {
  Task     *target = taskGet(KERNEL_TASK_ID);
  OpenFile *ret = fsOpenGeneric(filename, target, flags, mode);
  if (RET_IS_ERR((size_t)(ret)))
    return 0;
  return ret;
}

size_t fsUserOpen(void *task, char *filename, int flags, int mode) {
  if (flags & FASYNC) {
    debugf("[syscalls::fs] FATAL! Tried to open %s with O_ASYNC!\n", filename);
    return ERR(ENOSYS);
  }
  OpenFile *file = fsOpenGeneric(filename, (Task *)task, flags, mode);
  if (RET_IS_ERR((size_t)file))
    return (size_t)file;

  return file->id;
}

bool fsCloseGeneric(OpenFile *file, Task *task) {
  fsUnregisterNode(task, file);

  bool res = file->handlers->close ? file->handlers->close(file) : true;
  epollCloseNotify(file);
  free(file);
  return res;
}

bool fsKernelClose(OpenFile *file) {
  Task *target = taskGet(KERNEL_TASK_ID);
  return fsCloseGeneric(file, target);
}

size_t fsUserClose(void *task, int fd) {
  OpenFile *file = fsUserGetNode(task, fd);
  if (!file)
    return ERR(EBADF);
  bool res = fsCloseGeneric(file, (Task *)task);
  if (res)
    return 0;
  else
    return -1;
}

size_t fsGetFilesize(OpenFile *file) {
  return file->handlers->getFilesize(file);
}

size_t fsRead(OpenFile *file, uint8_t *out, uint32_t limit) {
  if (!file->handlers->read) {
    if (file->handlers->recvfrom) // we got a socket!
      return file->handlers->recvfrom(file, out, limit, 0, 0, 0);
    return ERR(EBADF);
  }
  return file->handlers->read(file, out, limit);
}

size_t fsWrite(OpenFile *file, uint8_t *in, uint32_t limit) {
  if (!(file->flags & O_RDWR) && !(file->flags & O_WRONLY))
    return ERR(EBADF);
  if (!file->handlers->write) {
    if (file->handlers->sendto) // we got a socket!
      return file->handlers->sendto(file, in, limit, 0, 0, 0);
    return ERR(EBADF);
  }
  return file->handlers->write(file, in, limit);
}

void fsReadFullFile(OpenFile *file, uint8_t *out) {
  fsRead(file, out, fsGetFilesize(file));
}

size_t fsUserSeek(void *task, uint32_t fd, int offset, int whence) {
  OpenFile *file = fsUserGetNode(task, fd);
  if (!file) // todo "special"
    return -1;
  int target = offset;
  if (whence == SEEK_SET)
    target += 0;
  else if (whence == SEEK_CURR)
    target += file->pointer;
  else if (whence == SEEK_END)
    target += fsGetFilesize(file);

  if (!file->handlers->seek)
    return ERR(ESPIPE);

  return file->handlers->seek(file, target, offset, whence);
}

size_t fsReadlink(void *task, char *path, char *buf, int size) {
  Task *target = task;
  spinlockAcquire(&target->infoFs->LOCK_FS);
  char *safeFilename = fsSanitize(target->infoFs->cwd, path);
  spinlockRelease(&target->infoFs->LOCK_FS);
  MountPoint *mnt = fsDetermineMountPoint(safeFilename);
  size_t      ret = -1;

  char *symlink = 0;
  if (!mnt->readlink) {
    free(safeFilename);
    return ERR(EINVAL);
  }
  char *strippedFilename = fsStripMountpoint(safeFilename, mnt);
  ret = mnt->readlink(mnt, strippedFilename, buf, size, &symlink);

  free(safeFilename);

  if (symlink) {
    char *symlinkResolved = fsResolveSymlink(mnt, symlink);
    free(symlink);
    ret = fsReadlink(task, symlinkResolved, buf, size);
    free(symlinkResolved);
  }
  return ret;
}

size_t fsMkdir(void *task, char *path, uint32_t mode) {
  Task *target = (Task *)task;
  spinlockAcquire(&target->infoFs->LOCK_FS);
  char *safeFilename = fsSanitize(target->infoFs->cwd, path);
  spinlockRelease(&target->infoFs->LOCK_FS);
  MountPoint *mnt = fsDetermineMountPoint(safeFilename);

  size_t ret = 0;

  char *symlink = 0;
  if (mnt->mkdir) {
    ret = mnt->mkdir(mnt, safeFilename, mode, &symlink);
  } else {
    ret = ERR(EROFS);
  }

  free(safeFilename);

  if (symlink) {
    char *symlinkResolved = fsResolveSymlink(mnt, symlink);
    free(symlink);
    ret = fsMkdir(task, symlinkResolved, mode);
    free(symlinkResolved);
  }

  return ret;
}

size_t fsUnlink(void *task, char *path, bool directory) {
  Task *target = (Task *)task;
  spinlockAcquire(&target->infoFs->LOCK_FS);
  char *safeFilename = fsSanitize(target->infoFs->cwd, path);
  spinlockRelease(&target->infoFs->LOCK_FS);
  MountPoint *mnt = fsDetermineMountPoint(safeFilename);

  size_t ret = 0;
  if (unixSocketUnlinkNotify(safeFilename)) {
    free(safeFilename);
    return 0;
  }

  char *symlink = 0;
  if (mnt->delete) {
    ret = mnt->delete(mnt, safeFilename, directory, &symlink);
  } else {
    ret = ERR(EROFS);
  }

  free(safeFilename);

  if (symlink) {
    char *symlinkResolved = fsResolveSymlink(mnt, symlink);
    free(symlink);
    ret = fsUnlink(task, symlinkResolved, directory);
    free(symlinkResolved);
  }

  return ret;
}

size_t fsLink(void *task, char *oldpath, char *newpath) {
  Task *target = (Task *)task;
  spinlockAcquire(&target->infoFs->LOCK_FS);
  char *oldpathSafe = fsSanitize(target->infoFs->cwd, oldpath);
  char *newpathSafe = fsSanitize(target->infoFs->cwd, newpath);
  spinlockRelease(&target->infoFs->LOCK_FS);
  MountPoint *mnt = fsDetermineMountPoint(oldpathSafe);
  if (fsDetermineMountPoint(newpathSafe) != mnt) {
    free(oldpathSafe);
    free(newpathSafe);
    return ERR(EXDEV);
  }

  size_t ret = 0;

  char *symlinkold = 0;
  char *symlinknew = 0;
  if (mnt->delete) {
    ret = mnt->link(mnt, oldpathSafe, newpathSafe, &symlinkold, &symlinknew);
  } else {
    ret = ERR(EPERM);
  }

  free(oldpathSafe);
  free(newpathSafe);

  char *old = oldpath;
  char *new = newpath;
  if (symlinkold)
    old = fsResolveSymlink(mnt, symlinkold);
  if (symlinknew)
    new = fsResolveSymlink(mnt, symlinknew);

  if (symlinkold)
    free(symlinkold);
  if (symlinknew)
    free(symlinknew);

  if (symlinkold || symlinknew)
    ret = fsLink(task, old, new);

  if (symlinkold)
    free(old);
  if (symlinknew)
    free(new);

  return ret;
}

// shared for fake filesystems etc
size_t fsSimpleSeek(OpenFile *file, size_t target, long int offset,
                    int whence) {
  // we're using the official ->pointer so no need to worry about much
  file->pointer = target;
  return 0;
}
