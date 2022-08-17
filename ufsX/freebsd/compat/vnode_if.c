//
//  lock.c
//  ufsX
//
//  Created by John Othwolo on 6/20/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>

#include <freebsd/compat/compat.h>

enum { NULLTHREAD = -1 };
typedef uint64_t tid_t;

#pragma mark - VNOP_ISLOCKED

int VNOP_ISLOCKED(struct vnode *vp)
{
//    struct vnop_islocked_args a;
//    struct vnode_compat *cvnode;
//
//    log_debug("enter");
//
//    a.a_vp = vp;
//    cvnode = &(vnode_fsnode(vp)[0]);
//
//    return cvnode->ops.vn_islocked(&a);
    return 0;
}

#pragma mark - VNOP_LOCK

int VNOP_LOCK1(struct vnode *vp, int flags, const char *file, int line)
{
//    struct vnop_lock1_args a;
//    struct vnode_compat *cvnode;
//
//    log_debug("VNOP_LOCK1 called from %s:%d", file, line);
//
//    a.a_vp = vp;
//    a.a_flags = flags;
//    a.a_file = file;
//    a.a_line = line;
//    cvnode = &(vnode_fsnode(vp)[0]);
//
//    return cvnode->ops.vn_lock1(&a);
    return 0;
}

#pragma mark - VNOP_UNLOCK

int VNOP_UNLOCK1(struct vnode *vp, const char *file, int line)
{
//    struct vnop_unlock_args a;
//    struct vnode_compat *cvnode;
//
//    log_debug("VNOP_UNLOCK1 called from %s:%d", file, line);
//
//    a.a_vp = vp;
//    a.a_file = file;
//    a.a_line = line;
//    cvnode = &(vnode_fsnode(vp)[0]);
//
//    return cvnode->ops.vn_unlock1(&a);
    return 0;
}


