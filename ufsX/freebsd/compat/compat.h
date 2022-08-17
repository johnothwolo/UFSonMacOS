//
//  compat.h
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef compat_h
#define compat_h

#include <sys/cdefs.h>
#include <kern/thread.h>
#include <kern/debug.h>
#include <libkern/OSAtomic.h>


#pragma mark - FFS vgetf func

struct vfs_context;
struct mount;
struct vnode;

kern_return_t  thread_terminate(thread_t);

#pragma mark - sys/systm.h (comes first...)
#include <freebsd/compat/systm.h>
#pragma mark - sys/atomic.h
#include <freebsd/compat/atomic.h>
#pragma mark - sys/buf.h
#include <freebsd/compat/buf.h>
#pragma mark - sys/bufobj.h
#include <freebsd/compat/bufobj.h>
#pragma mark - sys/debug.h
#include <freebsd/compat/debug.h>
#pragma mark - sys/dirent.h
#include <freebsd/compat/dirent.h>
#pragma mark - sys/lock.h
#include <freebsd/compat/lock.h>
#pragma mark - sys/malloc.h
#include <freebsd/compat/malloc.h>
#pragma mark - sys/mount.h
#include <freebsd/compat/mount.h>
#pragma mark - sys/param.h
#include <freebsd/compat/param.h>
#pragma mark - sys/stat.h
#include <freebsd/compat/stat.h>
#pragma mark - sys/vnode.h
#include <freebsd/compat/vnode.h>
#pragma mark - sys/conf.h
#include <freebsd/compat/conf.h>
#pragma mark - sys/gsb_crc32.h
#include <freebsd/compat/gsb_crc32.h>
#pragma mark - sys/time.h
#include <freebsd/compat/time.h>
#pragma mark - sys/taskqueue.h
#include <freebsd/compat/taskqueue.h>
#pragma mark -


#endif /* compat_h */
