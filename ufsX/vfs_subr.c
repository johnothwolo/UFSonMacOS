//
//  vfs_subr.c
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <libkern/OSDebug.h>

#include <stdatomic.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#include <freebsd/sys/compat.h>
#include <freebsd/sys/seqc.h>


#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>

#define    IGNORE_LOCK(vp) ((vp) == NULL ||        \
                            vnode_vtype(vp) == VCHR || vnode_vtype(vp) == VBAD)

int vfs_badlock_ddb = 1;    /* Drop into debugger on violation. */
SYSCTL_INT(_debug, OID_AUTO, vfs_badlock_ddb, CTLFLAG_RW, &vfs_badlock_ddb, 0,
    "Drop into debugger on lock violation");

int vfs_badlock_mutex = 1;    /* Check for interlock across VOPs. */
SYSCTL_INT(_debug, OID_AUTO, vfs_badlock_mutex, CTLFLAG_RW, &vfs_badlock_mutex,
    0, "Check for interlock across VOPs");

int vfs_badlock_print = 1;    /* Print lock violations. */
SYSCTL_INT(_debug, OID_AUTO, vfs_badlock_print, CTLFLAG_RW, &vfs_badlock_print,
    0, "Print lock violations");

int vfs_badlock_vnode = 1;    /* Print vnode details on lock violations. */
SYSCTL_INT(_debug, OID_AUTO, vfs_badlock_vnode, CTLFLAG_RW, &vfs_badlock_vnode,
    0, "Print vnode details on lock violations");

int vfs_badlock_backtrace = 1;    /* Print backtrace at lock violations. */
SYSCTL_INT(_debug, OID_AUTO, vfs_badlock_backtrace, CTLFLAG_RW,
    &vfs_badlock_backtrace, 0, "Print backtrace at lock violations");

static void
vfs_badlock(const char *msg, const char *str, struct vnode *vp)
{

    if (vfs_badlock_backtrace)
        OSReportWithBacktrace("vfs_badlock_backtrace");
    if (vfs_badlock_vnode)
        vn_printf(vp, "vnode ");
    if (vfs_badlock_print)
        ufs_debug("%s: %p %s\n", str, (void *)vp, msg);
    if (vfs_badlock_ddb)
        panic("lock violation");
}

void
assert_vi_locked(struct vnode *vp, const char *str)
{
    if (vfs_badlock_mutex && !(VTOI(vp)->i_interlock_owner == thread_tid(current_thread())))
        vfs_badlock("interlock is not locked but should be", str, vp);
}

void
assert_vi_unlocked(struct vnode *vp, const char *str)
{
    if (vfs_badlock_mutex && VTOI(vp)->i_interlock_owner == thread_tid(current_thread()))
        vfs_badlock("interlock is locked but should not be", str, vp);
}

void
assert_vop_locked(struct vnode *vp, const char *str)
{
    int locked;
    
    if (!IGNORE_LOCK(vp)) {
        locked = VOP_ISLOCKED(vp);
        if (locked == 0 || locked == LK_EXCLOTHER)
            vfs_badlock("is not locked but should be", str, vp);
    }
}

void
assert_vop_unlocked(struct vnode *vp, const char *str)
{
    if (!IGNORE_LOCK(vp) && VOP_ISLOCKED(vp) == LK_EXCLUSIVE)
        vfs_badlock("is locked but should not be", str, vp);
}

void
assert_vop_elocked(struct vnode *vp, const char *str)
{
    if (!IGNORE_LOCK(vp) && VOP_ISLOCKED(vp) != LK_EXCLUSIVE)
        vfs_badlock("is not exclusive locked but should be", str, vp);
}

int
buf_xflags(buf_t bp)
{
    return (int)((uintptr_t)buf_fsprivate(bp));
}

void
buf_setxflags(buf_t bp, int xflags)
{
    int b_xflags = buf_xflags(bp);
    buf_setfsprivate(bp, (void*)((uintptr_t)(b_xflags | xflags)));
}

void
buf_clrxflags(buf_t bp)
{
    buf_setfsprivate(bp, 0);
}

buf_t
incore(struct vnode *vp, daddr64_t bn)
{
    struct vfsstatfs *stat;
    struct buf *bp;
    
    stat = vfs_statfs(vnode_mount(vp));
    bp = buf_getblk(vp, bn, (int)stat->f_iosize, 0, 0, BLK_ONLYVALID);
    
    return bp;
}

void vn_seqc_write_begin(struct vnode *vp)
{
    if (!vp) return;
    struct inode *ip = VTOI(vp);
    
    VI_LOCK(vp);
    ASSERT_VI_LOCKED(vp, __func__);
    VNPASS(ip->i_seqc_users >= 0, vp);
    ip->i_seqc_users++;
    if (ip->i_seqc_users == 1)
        seqc_sleepable_write_begin(&ip->i_seqc);
    VI_UNLOCK(vp);
}

void vn_seqc_write_end(struct vnode *vp)
{
    if (!vp) return;
    struct inode *ip = VTOI(vp);
    
    VI_LOCK(vp);
    ASSERT_VI_LOCKED(vp, __func__);
    VNPASS(ip->i_seqc_users > 0, vp);
    ip->i_seqc_users--;
    if (ip->i_seqc_users == 0)
        seqc_sleepable_write_end(&ip->i_seqc);
        VI_UNLOCK(vp);

    
}
