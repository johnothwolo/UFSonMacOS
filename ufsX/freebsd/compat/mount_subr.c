//
//  vfs_mount.c
//  ufsX
//
//  Created by John Othwolo on 6/24/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <libkern/OSDebug.h>

#include <stdatomic.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>

#include <freebsd/compat/compat.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>

#pragma mark - BE CAREFULE USING THESE FUNCS. THEY MAP TO THE SPCIFIC FLAGS

/*
 description
 */

/// This
/// @param flag  is the flags you want to check
static inline int check_kernflag_common(struct bsdmount *bmp, int flag, int locked)
{
    int ret;
    if (!locked) MNT_ILOCK(bmp);
    ret = bmp->mnt_kern_flag & flag;
    if (!locked) MNT_IUNLOCK(bmp);
    return ret;
}


bool vfs_issoftdep(mount_t mp, int locked)
{
    if (mp == NULL) false; // just assume this ever happens
    return check_kernflag_common(VFSTOUFS(mp)->um_compat, MNTK_SOFTDEP, locked);
}

bool vfs_issuspend(mount_t mp, int locked)
{
    if (mp == NULL) false;
    return check_kernflag_common(VFSTOUFS(mp)->um_compat, MNTK_SUSPEND, locked);
}

bool vfs_issuspend2(mount_t mp, int locked)
{
    if (mp == NULL) false;
    return check_kernflag_common(VFSTOUFS(mp)->um_compat, MNTK_SUSPEND2, locked);
}

bool vfs_issuspended(mount_t mp, int locked)
{
    if (mp == NULL) false;
    return check_kernflag_common(VFSTOUFS(mp)->um_compat, MNTK_SUSPENDED, locked);
}

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
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    MNT_ILOCK(bmp);
    bmp->mnt_vfs_ops++;
    if (bmp->mnt_vfs_ops > 1) {
        MNT_IUNLOCK(bmp);
        return;
    }
    
    if (bmp->mnt_ref <= 0 || bmp->mnt_writeopcount < 0)
        panic("%s: invalid count(s) on mp %p: ref %d writeopcount %d\n",
              __func__, mp, bmp->mnt_ref, bmp->mnt_writeopcount);
    MNT_IUNLOCK(bmp);
}

void
vfs_op_exit_locked(struct bsdmount *bmp)
{
    
    if (bmp->mnt_vfs_ops <= 0){
        panic("%s: invalid vfs_ops count %d for mp %p\n", __func__, bmp->mnt_vfs_ops, bmp);
    }
    bmp->mnt_vfs_ops--;
}

void
vfs_op_exit(struct mount *mp)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    MNT_ILOCK(bmp);
    vfs_op_exit_locked(bmp);
    MNT_IUNLOCK(bmp);
}

void
vfs_dump_mount_counters(struct mount *mp)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;

    log_debug("%s: mp %p vfs_ops %d\n", __func__, bmp, bmp->mnt_vfs_ops);
    log_debug("        ref : %d\n", bmp->mnt_ref);
    log_debug("    lockref : %s\n", "(Unsupported)");
    log_debug("writeopcount: %d\n", bmp->mnt_writeopcount);
    log_debug("counter       struct total\n");
    log_debug("ref             %-5d \n", bmp->mnt_ref);
    log_debug("lockref         %s   \n", "(Unsupported)");
    log_debug("writeopcount    %-5d \n", bmp->mnt_writeopcount);

    panic("invalid counts on struct mount");
}

uint64_t
vfs_freebsdflags(mount_t mp)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    uint64_t ret;
    
    MNT_ILOCK(bmp);
    ret = bmp->mnt_flag;
    MNT_IUNLOCK(bmp);
    return ret;
}

void
vfs_setfreebsdflags(mount_t mp, uint64_t flags)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    uint64_t freeflags;
    
    // get supported darwin flags and call vfs_setflags()
    freeflags = (flags | (FREEBSD_MNT_VISFLAGMASK));

    MNT_ILOCK(bmp);
    bmp->mnt_flag |= freeflags;
    MNT_IUNLOCK(bmp);
}
// very similar to vfs_setfreebsdflags()
void
vfs_clearflags_fbsd(mount_t mp, uint64_t flags)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;
    uint64_t freeflags;
    
    // get supported darwin flags and call vfs_setflags()
    freeflags = (flags | (FREEBSD_MNT_VISFLAGMASK));

    MNT_ILOCK(bmp);
    bmp->mnt_flag &= ~freeflags;
    MNT_IUNLOCK(bmp);
    
}

void
vfs_ref(struct mount *mp)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;

    MNT_ILOCK(bmp);
    MNT_REF(bmp);
    MNT_IUNLOCK(bmp);
}

void
vfs_rel(struct mount *mp)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    struct bsdmount *bmp = VFSTOUFS(mp)->um_compat;

    MNT_ILOCK(bmp);
    MNT_REL(bmp);
    MNT_IUNLOCK(bmp);
}

#define vfs_busy_xnu    vfs_busy
#define vfs_unbusy_xnu  vfs_unbusy

int  vfs_busy_bsd(mount_t mp, int flags)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    vfs_ref(mp);
    return vfs_busy_xnu(mp, flags);
}

void vfs_unbusy_bsd(mount_t mp)
{
    ASSERT(mp != NULL, ("mount point is NULL"));
    vfs_rel(mp);
    vfs_unbusy_xnu(mp);
}

char *devtoname(struct vnode *devvp)
{
    return vfs_statfs(vnode_mountedhere(devvp))->f_mntfromname;
}
