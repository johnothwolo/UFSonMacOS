//
//  vfs_vnops.c
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#define MACH_ASSERT 1

#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/mount.h>

#include <freebsd/compat/compat.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ffs/fs.h>


void vfs_dump_mount_counters(struct mount *mp);
void vfs_op_exit_locked(struct bsdmount *ump);


static int
vn_get_ino_alloc_vget(struct mount *mp, void *arg, int lkflags,
    struct vnode **rvp)
{
    struct vfs_vget_args vargs = { .flags = lkflags };
    return (VFS_VGET(mp, *(ino_t *)arg, &vargs, rvp, vfs_context_create(NULL)));
}

int
vn_vget_ino(struct vnode *vp, ino_t ino, int lkflags, struct vnode **rvp)
{

    return (vn_vget_ino_gen(vp, vn_get_ino_alloc_vget, &ino,
        lkflags, rvp));
}

int
vn_vget_ino_gen(struct vnode *vp, vn_get_ino_t alloc, void *alloc_arg,
    int lkflags, struct vnode **rvp)
{
    struct mount *mp;
    int error;

    mp = vnode_mount(vp);
    error = vfs_busy(mp, LK_NOWAIT);
    if (error) {
        // if we can't 'busy' immediately just busy with wait
        error = vfs_busy(mp, 0);
        if (error != 0)
            return (ENOENT);
        if (vnode_isrecycled(vp)) {
            vfs_unbusy(mp);
            return (ENOENT);
        }
    }
    error = alloc(mp, alloc_arg, lkflags, rvp);
    vfs_unbusy(mp);
    if (vnode_isrecycled(vp)) {
        if (error == 0) {
            if (*rvp == vp)
                vnode_rele(vp);
            else
                vnode_put(*rvp);
        }
        error = ENOENT;
    }
    return (error);
}

#pragma mark -
#pragma mark Write suspension support

#define TDP_IGNSUSP    0x00800000 /* Permission to ignore the MNTK_SUSPEND* */

/*
 * Preparing to start a filesystem write operation. If the operation is
 * permitted, then we bump the count of operations in progress and
 * proceed. If a suspend request is in progress, we wait until the
 * suspension is over, and then proceed.
 */
// MARK: vn_start_write_refed()
static int
vn_start_write_refed(struct mount *mp, int flags, bool mplocked)
{
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    int error = 0;
    
    if (__probable(!mplocked) && (flags & V_XSLEEP) == 0) {
        MPASS((bmp->mnt_kern_flag & MNTK_SUSPEND) == 0);
        // freebsd uses some complex smr macros to handle these vars
        // I think atomic_add is enough
        OSIncrementAtomic(&bmp->mnt_writeopcount);
        return (0);
    }
    
    // lock ufsmount if not locked
    if(mplocked){
        LCK_MTX_ASSERT(bmp->mnt_lock, LCK_MTX_ASSERT_OWNED);
    } else {
        MNT_ILOCK(bmp);
    }
    
    if(bmp->mnt_susp_owner != current_thread()){
        while ((bmp->mnt_kern_flag & MNTK_SUSPEND) != 0) {
            if (flags & V_NOWAIT) {
                error = EWOULDBLOCK;
                goto unlock;
            }
            error = msleep(&bmp->mnt_flag, bmp->mnt_lock, PCATCH, "suspfs", 0);
            if (error)
                goto unlock;
        }
    }
    
    if (flags & V_XSLEEP)
        goto unlock;
    
unlock:
    if (error != 0 || (flags & V_XSLEEP) != 0)
        MNT_REL(bmp);
    
    // don't unlock if we entered her locked
    if (!mplocked)
        MNT_IUNLOCK(bmp);
    return error;
}

int
vn_start_write(struct vnode *vp, struct mount **mpp, int flags)
{
    struct mount *mp;
    
    ASSERT((flags & V_MNTREF) == 0 || (*mpp != NULL && vp == NULL), ("V_MNTREF requires mp"));

    /*
     * If a vnode is provided, get and return the mount point that
     * to which it will write.
     */
    // FIXME: vnode_mount() won't fail in osx
    if (vp != NULL) {
        mp = vnode_mount(vp);
        if(!vfs_isunmount(mp)){
            *mpp = NULL;
            return (0);
        }
    }
    
    if ((mp = *mpp) == NULL)
        return (0);

    return (vn_start_write_refed(mp, flags, false));
}

/*
 * Secondary suspension. Used by operations such as vop_inactive
 * routines that are needed by the higher level functions. These
 * are allowed to proceed until all the higher level functions have
 * completed (indicated by mnt_writeopcount dropping to zero). At that
 * time, these operations are halted until the suspension is over.
 */
int
vn_start_secondary_write(struct vnode *vp, __unused struct mount **mpp, int flags)
{
    struct inode *ip = VTOI(vp);
    struct bsdmount *bmp = ITOUMP(ip)->um_compat;
    struct mount *mp = NULL;
    int error = 0;

    ASSERT((flags & V_MNTREF) == 0 || (*mpp != NULL && vp == NULL), ("V_MNTREF requires mp"));
    
    if (vp != NULL){
        // if vnode is provided, use its mountpoint
        *mpp = vnode_mount(vp);
        vfs_ref(mp);
    }
    
    /*
     * If we are not suspended or have not yet reached suspended
     * mode, then let the operation proceed.
     */
    if ((mp = *mpp) == NULL)
        return (0);
    
retry:
    MNT_ILOCK(bmp);
    
    /*
     * VOP_GETWRITEMOUNT() returns with the mp refcount held through
     * a vfs_ref().
     * As long as a vnode is not provided we need to acquire a
     * refcount for the provided mountpoint too, in order to
     * emulate a vfs_ref().
     */
    if (vp == NULL && (flags & V_MNTREF) == 0) {
        MNT_REF(bmp);
    }
    
    /* if mount isn't suspended, increment writes and return successfully */
    if ((bmp->mnt_kern_flag & (MNTK_SUSPENDED | MNTK_SUSPEND2)) == 0) {
        bmp->mnt_secondary_writes++;
        bmp->mnt_secondary_accwrites++;
        return (0);
    }
    
    if (flags & V_NOWAIT) {
        MNT_REL(bmp); // we own the lock here.
        MNT_IUNLOCK(bmp);
        return (EWOULDBLOCK);
    }
    
    /* Wait for the suspension to finish. */
    error = msleep(&bmp->mnt_flag, bmp->mnt_lock,  PDROP | PVFS, "suspfs", NULL);
    
    vfs_rel(mp); // drop reference count
    
    if (error == 0)
        goto retry;
    return error;
}

/*
 * Filesystem write operation has completed. If we are suspending and this
 * operation is the last one, notify the suspender that the suspension is
 * now in effect.
 */
void
vn_finished_write(struct mount *mp)
{
    struct bsdmount *bmp;
    int c;
    
    if (mp == NULL)
        return;
    
    bmp = VFSTOUFS(mp)->um_compat;
    
    MNT_ILOCK(bmp);
    c = --bmp->mnt_writeopcount;
    if (bmp->mnt_vfs_ops == 0) {
        MPASS((bmp->mnt_kern_flag & MNTK_SUSPEND) == 0);
        MNT_IUNLOCK(bmp);
        return;
    }
    if (c < 0)
        vfs_dump_mount_counters(mp); // panic...
    if ((bmp->mnt_kern_flag & MNTK_SUSPEND) != 0 && c == 0)
        wakeup(&bmp->mnt_writeopcount);
    MNT_IUNLOCK(bmp);
}

/*
 * Filesystem secondary write operation has completed. If we are
 * suspending and this operation is the last one, notify the suspender
 * that the suspension is now in effect.
 */
void
vn_finished_secondary_write(struct mount *mp)
{
    struct bsdmount *bmp;
    
    if (mp == NULL)
        return;
    
    bmp = VFSTOUFS(mp)->um_compat;
    
    MNT_ILOCK(bmp);
    bmp->mnt_secondary_writes--;
    if (bmp->mnt_secondary_writes < 0)
        panic("vn_finished_secondary_write: neg cnt");
    // TODO: why '<=' 0? doesn't it already panic if < 0?
    if ((bmp->mnt_kern_flag & MNTK_SUSPEND) != 0 && bmp->mnt_secondary_writes <= 0)
        wakeup(&bmp->mnt_secondary_writes);
    MNT_IUNLOCK(bmp);
}

int
vfs_set_susupendowner(struct mount *mp, thread_t td)
{
    struct bsdmount *bmp;
    
    if (mp == NULL)
        return EINVAL;

    bmp = VFSTOUFS(mp)->um_compat;
    
    vfs_op_enter(mp); // careful, this acquires the lock
    MNT_ILOCK(bmp);
    
    if (bmp->mnt_susp_owner == current_thread()) {
        vfs_op_exit_locked(bmp);
        MNT_IUNLOCK(bmp);
        return (EALREADY);
    }
    
    bmp->mnt_susp_owner = td != 0 ? td : current_thread();
    
    MNT_IUNLOCK(bmp);
    vfs_op_exit(mp); // we call this here.
    
    return 0;
}

/*
 * Request a filesystem to suspend write operations.
 */
int
vfs_write_suspend(struct mount *mp, thread_t td, int flags)
{

    int error;
    struct bsdmount *bmp;
    
    if (mp == NULL)
        return EINVAL;
    
    vfs_op_enter(mp); // careful, this acquires the lock

    bmp = VFSTOUFS(mp)->um_compat;
    
    MNT_ILOCK(bmp);
    if (bmp->mnt_susp_owner == current_thread()) {
        vfs_op_exit_locked(bmp);
        MNT_IUNLOCK(bmp);
        return (EALREADY);
    }
    
    while (bmp->mnt_kern_flag & MNTK_SUSPEND)
        msleep(&bmp->mnt_flag, bmp->mnt_lock, PUSER - 1, "wsuspfs", 0);
    
    if ((flags & VS_SKIP_UNMOUNT) != 0 && (vfs_isforce(mp) && vfs_isunmount(mp)) != 0) {
        vfs_op_exit_locked(bmp);
        MNT_IUNLOCK(bmp);
        return (EBUSY);
    }
    
    bmp->mnt_kern_flag |= MNTK_SUSPEND;
    bmp->mnt_susp_owner = td != 0 ? td : current_thread();
    if (bmp->mnt_writeopcount > 0)
        (void) msleep(&bmp->mnt_writeopcount, bmp->mnt_lock, (PUSER - 1)|PDROP, "suspwt", 0);
    else
        MNT_IUNLOCK(bmp);
    if ((error = VFS_SYNC(mp, MNT_SUSPEND)) != 0) {
        vfs_write_resume(mp, 0);
        /* vfs_write_resume does vfs_op_exit() for us */
    }
    return (error);
}


/*
 * Request a filesystem to resume write operations.
 */
void
vfs_write_resume(struct mount *mp, int flags)
{
    struct bsdmount *bmp;
    if (mp == NULL)
        return;
    bmp = VFSTOUFS(mp)->um_compat;

    MNT_ILOCK(bmp);
    if ((bmp->mnt_kern_flag & MNTK_SUSPEND) != 0) {
        ASSERT(bmp->mnt_susp_owner == current_thread(), ("mnt_susp_owner"));
        bmp->mnt_kern_flag &= ~(MNTK_SUSPEND | MNTK_SUSPEND2 | MNTK_SUSPENDED);
        bmp->mnt_susp_owner = NULL;
        wakeup(&bmp->mnt_writeopcount);
        wakeup(&bmp->mnt_flag);
        if ((flags & VR_START_WRITE) != 0) {
            MNT_REF(bmp);
            bmp->mnt_writeopcount++;
        }
        MNT_IUNLOCK(bmp);
        
        if ((flags & VR_NO_SUSPCLR) == 0)
            VFS_SUSP_CLEAN(mp);
        
        vfs_op_exit(mp);
        
    } else if ((flags & VR_START_WRITE) != 0) {
        MNT_REF(bmp);
        vn_start_write_refed(mp, 0, true);
    } else {
        MNT_IUNLOCK(bmp);
    }
}

/*
 * Helper loop around vfs_write_suspend() for filesystem unmount VFS
 * methods.
 */
int
vfs_write_suspend_umnt(struct mount *mp)
{
    if (mp == NULL) return EINVAL;
    
    int error;
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    
    ASSERT(bmp->mnt_susp_owner != current_thread(), ("vfs_write_suspend_umnt: recursed"));

    /* dounmount() already called vn_start_write(). */
    for (;;) {
        vn_finished_write(mp);
        error = vfs_write_suspend(mp, 0, 0);
        if (error != 0) {
            vn_start_write(NULL, &mp, V_WAIT);
            return (error);
        }
        MNT_ILOCK(bmp);
        if ((bmp->mnt_kern_flag & MNTK_SUSPENDED) != 0)
            break;
        MNT_IUNLOCK(bmp);
        vn_start_write(NULL, &mp, V_WAIT);
    }
    bmp->mnt_kern_flag &= ~(MNTK_SUSPENDED | MNTK_SUSPEND2);
    wakeup(&bmp->mnt_kern_flag);
    MNT_IUNLOCK(bmp);
    return (0);
}

#pragma mark -
