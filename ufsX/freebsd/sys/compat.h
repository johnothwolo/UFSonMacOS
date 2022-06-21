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

#define    D_VERSION    D_VERSION_04


#pragma mark - debugging

// TODO: move this to a "debug.h"
__private_extern__ void __ufs_debug(thread_t, const char*, int, const char*, const char*, ...) __printflike(5, 6);
#define ufs_debug(fmt, a...) __ufs_debug(current_thread(), __FILE__, __LINE__, __func__, fmt, ##a)


#pragma mark - FFS static func (which I un-static-ed)

__private_extern__ int
ffs_vgetf( struct mount *mp, ino_t ino, int flags,
              struct vnode **vpp, int ffs_flags, vfs_context_t context);

#pragma mark - sys/systm.h (comes first...)
#include <freebsd/sys/systm.h>
#pragma mark - sys/atomic.h
#include <freebsd/sys/atomic.h>
#pragma mark - sys/buf.h
#include <freebsd/sys/buf.h>
#pragma mark - sys/debug.h
#include <freebsd/sys/debug.h>
#pragma mark - sys/dirent.h
#include <freebsd/sys/dirent.h>
#pragma mark - sys/lock.h
#include <freebsd/sys/lock.h>
#pragma mark - sys/malloc.h
#include <freebsd/sys/malloc.h>
#pragma mark - sys/mount.h
#include <freebsd/sys/mount.h>
#pragma mark - sys/param.h
#include <freebsd/sys/param.h>
#pragma mark - sys/seqc.h
#include <freebsd/sys/seqc.h>
#pragma mark - sys/stat.h
#include <freebsd/sys/stat.h>
#pragma mark - sys/vnode.h
#include <freebsd/sys/vnode.h>
#pragma mark - sys/conf.h
#include <freebsd/sys/conf.h>
#pragma mark - sys/gsb_crc32.h
#include <freebsd/sys/gsb_crc32.h>
#pragma mark -


#endif /* compat_h */
