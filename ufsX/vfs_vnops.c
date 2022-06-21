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

#include <freebsd/sys/compat.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ffs/fs.h>


int ffs_sync(struct mount *mp, int waitfor);

#define VFS_SYNC ffs_sync

void vfs_dump_mount_counters(struct mount *mp);
void vfs_op_enter(struct mount *mp);
void vfs_op_exit_locked(struct ufsmount *ump);
void vfs_op_exit(struct mount *mp);


static int
vn_get_ino_alloc_vget(struct mount *mp, void *arg, int lkflags,
    struct vnode **rvp)
{

    return (VFS_VGET(mp, *(ino_t *)arg, lkflags, rvp, vfs_context_create(NULL)));
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
    error = vfs_busy_fbsd(mp, MBF_NOWAIT);
    do {
        // if we can't 'busy' immediately just sleep
        error = vfs_busy_fbsd(mp, 0);
        if (error != 0)
            return (ENOENT);
        if (vnode_isrecycled(vp)) {
            vfs_unbusy(mp);
            return (ENOENT);
        }
    } while (0);
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

// ??? FIXME: Don't forget this driver handles multiple filesystems!!!
// FIXME: URGENT: create a filsystem hashmap that store filesystem thread_local variables instead (like what we did in locks).
static _Thread_local unsigned td_pflags_gt = 0; // gt is for 'thread global' (reversed like french)

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
    struct ufsmount *ump;
    int error = 0;
    
    ump = VFSTOUFS(mp);
    
    if (__probable(!mplocked) && (flags & V_XSLEEP) == 0) {
        MPASS((ump->um_kern_flag & MNTK_SUSPEND) == 0);
        // freebsd uses some complex thread macros to increment these vars
        // I think atomic_add is enough
        OSIncrementAtomic(&ump->um_writeopcount);
        return (0);
    }
    
    // lock ufsmount if not locked
    if(mplocked){
        LCK_MTX_ASSERT(UFS_MTX(ump), LCK_MTX_ASSERT_OWNED);
    } else {
        UFS_LOCK(ump);
    }
    
    if((td_pflags_gt & TDP_IGNSUSP) == 0 && ump->um_susp_owner != current_thread()){
        while ((ump->um_kern_flag & MNTK_SUSPEND) != 0) {
            if (flags & V_NOWAIT) {
                error = EWOULDBLOCK;
                goto unlock;
            }
            error = msleep(&ump->um_kern_flag, UFS_MTX(ump), PCATCH, "suspfs", 0);
            if (error)
                goto unlock;
        }
    }
    
    if (flags & V_XSLEEP)
        goto unlock;
    
unlock:
    if (error != 0 || (flags & V_XSLEEP) != 0)
        UFS_REL(ump);
    UFS_UNLOCK(ump);
    return error;
}

int
vn_start_write(struct vnode *vp, struct mount **mpp, int flags)
{
    struct mount *mp;
    
    KASSERT((flags & V_MNTREF) == 0 || (*mpp != NULL && vp == NULL), ("V_MNTREF requires mp"));

    /*
     * If a vnode is provided, get and return the mount point that
     * to which it will write.
     */
    if (vp != NULL) {
        if ((mp = vnode_mount(vp)) == NULL) {
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
vn_start_secondary_write(struct vnode *vp, struct mount **mpp, int flags)
{
    struct inode *ip = VTOI(vp);
    struct ufsmount *ump = ITOUMP(ip);
    struct mount *mp;
    int error = 0;
    
    if (vp != NULL) {
        if ((mp = vnode_mount(vp)) == NULL) {
            *mpp = NULL;
            return (0);
        }
    }
    
    /*
     * If we are not suspended or have not yet reached suspended
     * mode, then let the operation proceed.
     */
    if ((mp = *mpp) == NULL)
        return (0);
    
    UFS_LOCK(ump);

    if (flags & V_NOWAIT) {
        return (EWOULDBLOCK);
    }
    if ((ump->um_kern_flag & (MNTK_SUSPENDED | MNTK_SUSPEND2)) == 0) {
        ump->um_secondary_writes++;
        ump->um_secondary_accwrites++;
        return (0);
    }
    /*
     * Wait for the suspension to finish.
     */
    // FIXME: examine this part... with locks...
    msleep(&ump->um_kern_flag, UFS_MTX(ump),  PCATCH | PINOD, "suspfs", NULL);
    
    UFS_LOCK(ump);
    
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
    struct ufsmount *ump;
    int c;
    
    if (mp == NULL)
        return;
    
    ump = VFSTOUFS(mp);
    
    UFS_LOCK(ump);
    c = --ump->um_writeopcount;
    if (ump->um_vfs_ops == 0) {
        MPASS((ump->um_kern_flag & MNTK_SUSPEND) == 0);
        UFS_UNLOCK(ump);
        return;
    }
    if (c < 0)
        vfs_dump_mount_counters(mp);
    if ((ump->um_kern_flag & MNTK_SUSPEND) != 0 && c == 0)
        wakeup(&ump->um_writeopcount);
    UFS_UNLOCK(ump);
}

/*
 * Filesystem secondary write operation has completed. If we are
 * suspending and this operation is the last one, notify the suspender
 * that the suspension is now in effect.
 */
void
vn_finished_secondary_write(struct mount *mp)
{
    struct ufsmount *ump;
    if (mp == NULL)
        return;
    ump = VFSTOUFS(mp);
    
    UFS_LOCK(ump);
    ump->um_secondary_writes--;
    if (ump->um_secondary_writes < 0)
        panic("vn_finished_secondary_write: neg cnt");
    // TODO: why '<=' 0? doesn't it already panic if < 0?
    if ((ump->um_kern_flag & MNTK_SUSPEND) != 0 && ump->um_secondary_writes <= 0)
        wakeup(&ump->um_secondary_writes);
    UFS_UNLOCK(ump);
}

/*
 * Request a filesystem to suspend write operations.
 */
int
vfs_write_suspend(struct mount *mp, int flags)
{

    int error;
    struct ufsmount *ump;
    
    if (mp == NULL)
        return EINVAL;
    
    vfs_op_enter(mp); // careful, this acquires the lock

    ump = VFSTOUFS(mp);
    
    UFS_LOCK(ump);
    if (ump->um_susp_owner == current_thread()) {
        vfs_op_exit_locked(ump);
        UFS_UNLOCK(ump);
        return (EALREADY);
    }
    
    while (ump->um_kern_flag & MNTK_SUSPEND)
        msleep(&ump->um_kern_flag, UFS_MTX(ump), PUSER - 1, "wsuspfs", 0);
    
    if ((flags & VS_SKIP_UNMOUNT) != 0 && (vfs_isforce(mp) && vfs_isunmount(mp)) != 0) {
        vfs_op_exit_locked(ump);
        UFS_UNLOCK(ump);
        return (EBUSY);
    }
    
    ump->um_kern_flag |= MNTK_SUSPEND;
    ump->um_susp_owner = current_thread();
    if (ump->um_writeopcount > 0)
        (void) msleep(&ump->um_writeopcount, UFS_MTX(ump), (PUSER - 1)|PDROP, "suspwt", 0);
    else
        UFS_UNLOCK(ump);
    if ((error = VFS_SYNC(mp, FBSD_MNT_SUSPEND)) != 0) {
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
    struct ufsmount *ump;
    if (mp == NULL)
        return;
    ump = VFSTOUFS(mp);

    UFS_LOCK(ump);
    if ((ump->um_kern_flag & MNTK_SUSPEND) != 0) {
        KASSERT(ump->um_susp_owner == current_thread(), ("mnt_susp_owner"));
        ump->um_kern_flag &= ~(MNTK_SUSPEND | MNTK_SUSPEND2 | MNTK_SUSPENDED);
        ump->um_susp_owner = NULL;
        wakeup(&ump->um_writeopcount);
        wakeup(&ump->um_kern_flag);
        td_pflags_gt &= ~TDP_IGNSUSP;
        if ((flags & VR_START_WRITE) != 0) {
            UFS_REF(mp);
            ump->um_writeopcount++;
        }
        UFS_UNLOCK(ump);
        if ((flags & VR_NO_SUSPCLR) == 0)
            VFS_SUSP_CLEAN(mp);
        vfs_op_exit(mp);
    } else if ((flags & VR_START_WRITE) != 0) {
        UFS_REF(mp);
        vn_start_write_refed(mp, 0, true);
    } else {
        UFS_UNLOCK(ump);
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
    struct ufsmount *ump = VFSTOUFS(mp);
    
    KASSERT((td_pflags_gt & TDP_IGNSUSP) == 0, ("vfs_write_suspend_umnt: recursed"));

    /* dounmount() already called vn_start_write(). */
    for (;;) {
        vn_finished_write(mp);
        error = vfs_write_suspend(mp, 0);
        if (error != 0) {
            vn_start_write(NULL, &mp, V_WAIT);
            return (error);
        }
        UFS_LOCK(ump);
        if ((ump->um_kern_flag & MNTK_SUSPENDED) != 0)
            break;
        UFS_UNLOCK(ump);
        vn_start_write(NULL, &mp, V_WAIT);
    }
    ump->um_kern_flag &= ~(MNTK_SUSPENDED | MNTK_SUSPEND2);
    wakeup(&ump->um_kern_flag);
    UFS_UNLOCK(ump);
    td_pflags_gt |= TDP_IGNSUSP;
    return (0);
}

#pragma mark -
#pragma mark These belong in vfs_mount.c

// MARK: FBSD to XNU flag conversion
DECLARE_MULTIFLAG_CMAP_FUNC(darwin_to_fbsd, mntflag, FBSD_MNT);
DECLARE_MULTIFLAG_CMAP_FUNC(fbsd_to_darwin, mntflag, FBSD_MNT);

//darwin_to_fbsd_mntflags(){
//
//}

/*
 * There are various reference counters associated with the mount point.
 * Normally it is permitted to modify them without taking the mnt ilock,
 * but this behavior can be temporarily disabled if stable value is needed
 * or callers are expected to block (e.g. to not allow new users during
 * forced unmount).
 */
// we don't keep per-cpu data here, not sure if that'll change anything though.
void
vfs_op_enter(struct mount *mp)
{
    struct ufsmount *ump;
    if (mp == NULL)
        return;
    ump = VFSTOUFS(mp);
    UFS_LOCK(ump);
    ump->um_vfs_ops++;
    if (ump->um_vfs_ops > 1) {
        UFS_UNLOCK(ump);
        return;
    }
    
    if (ump->um_ref <= 0 || ump->um_writeopcount < 0)
        panic("%s: invalid count(s) on mp %p: ref %d writeopcount %d\n",
            __func__, mp, ump->um_ref, ump->um_writeopcount);
    UFS_UNLOCK(ump);
}

void
vfs_op_exit_locked(struct ufsmount *ump)
{

    if (ump->um_vfs_ops <= 0){
        panic("%s: invalid vfs_ops count %d for mp %p\n",
              __func__, ump->um_vfs_ops, ump);
    }
    ump->um_vfs_ops--;
}

void
vfs_op_exit(struct mount *mp)
{
    struct ufsmount *ump;
    
    if (mp == NULL)
        return;
    
    ump = VFSTOUFS(mp);
    
    UFS_LOCK(ump);
    vfs_op_exit_locked(ump);
    UFS_UNLOCK(ump);
}

void
vfs_dump_mount_counters(struct mount *mp)
{
    if (mp == NULL)
        return;
    
    struct ufsmount *ump = VFSTOUFS(mp);

    printf("%s: mp %p vfs_ops %d\n", __func__, ump, ump->um_vfs_ops);
    printf("        ref : %d\n", ump->um_ref);
    printf("    lockref : %s\n", "(Unsupported)");
    printf("writeopcount: %d\n", ump->um_writeopcount);
    printf("counter       struct total\n");
    printf("ref             %-5d \n", ump->um_ref);
    printf("lockref         %s   \n", "(Unsupported)");
    printf("writeopcount    %-5d \n", ump->um_writeopcount);

    panic("invalid counts on struct mount");
}

uint64_t
vfs_flags_fbsd(mount_t mp)
{
    if (mp == NULL) return 0; // return 0 flags
    struct ufsmount *ump = VFSTOUFS(mp);

    uint64_t darwinflags, fbsdflags;
    
    darwinflags = vfs_flags(mp); // get darwin flags
    fbsdflags = darwin_to_fbsd_mntflag(darwinflags, (FBSD_MNT_CMDFLAGS | FBSD_MNT_VISFLAGMASK)); // convert supported ones
    // return the darwin and fbsd flags or'ed together
    return ump->um_kern_flag | fbsdflags;
}
void
vfs_setflags_fbsd(mount_t mp, uint64_t flags)
{
    if (mp == NULL) return;
    struct ufsmount *ump = VFSTOUFS(mp);
    uint64_t darwinflags, fbsdflags;
    
    // get supported darwin flags and call vfs_setflags()
    darwinflags = fbsd_to_darwin_mntflag(flags, (MNT_CMDFLAGS | MNT_VISFLAGMASK));
    vfs_setflags(mp, darwinflags);
    
    // set the freebsd-specific flags
    // filter out bad flags
    fbsdflags = flags & (FBSD_MNT_CMDFLAGS | FBSD_MNT_VISFLAGMASK);
    UFS_LOCK(ump);
    ump->um_kern_flag |= fbsdflags;
    UFS_UNLOCK(ump);
}
// very similar to vfs_setflags_fbsd()
void
vfs_clearflags_fbsd(mount_t mp, uint64_t flags)
{
    if (mp == NULL) return;
    struct ufsmount *ump = VFSTOUFS(mp);
    uint64_t darwinflags, fbsdflags;
    
    // get supported darwin flags and call vfs_setflags()
    darwinflags = fbsd_to_darwin_mntflag(flags, (MNT_CMDFLAGS | MNT_VISFLAGMASK));
    vfs_clearflags(mp, darwinflags);
    
    // set the freebsd-specific flags
    // filter out bad flags
    fbsdflags = flags & (FBSD_MNT_CMDFLAGS | FBSD_MNT_VISFLAGMASK);
    UFS_LOCK(ump);
    ump->um_kern_flag &= ~fbsdflags;
    UFS_UNLOCK(ump);
    
}

void
vfs_ref(struct mount *mp)
{
    if (mp == NULL) return;
    struct ufsmount *ump = VFSTOUFS(mp);

    UFS_LOCK(ump);
    UFS_REF(ump);
    UFS_UNLOCK(ump);
}

void
vfs_rel(struct mount *mp)
{
    if (mp == NULL) return;
    struct ufsmount *ump = VFSTOUFS(mp);

    UFS_LOCK(ump);
    UFS_REL(ump);
    UFS_UNLOCK(ump);
}

#define vfs_busy_xnu    vfs_busy
#define vfs_unbusy_xnu  vfs_unbusy

int  vfs_busy_fbsd(mount_t mp, int flags)
{
    if (mp == NULL) return EINVAL;
    vfs_ref(mp);
    return vfs_busy_xnu(mp, flags);
}

void vfs_unbusy_fbsd(mount_t mp)
{
    if (mp == NULL) return;
    vfs_rel(mp);
    vfs_unbusy_xnu(mp);
}
