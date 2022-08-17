//
//  misc_subr.c
//  ufsX
//
//  Created by John Othwolo on 7/27/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/ucred.h>

#include <freebsd/compat/mount.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ffs/fs.h>

bool DOINGSOFTDEP(struct vnode *vp)
{
    return MOUNTEDSOFTDEP(vnode_mount(vp));
}

bool MOUNTEDSOFTDEP(struct mount *mp)
{
    struct ufsmount *ump = VFSTOUFS(mp);
    return (ump->um_compat->mnt_flag & (FREEBSD_MNT_SOFTDEP | FREEBSD_MNT_SUJ)) != 0;
}

bool DOINGSUJ(struct vnode *vp)
{
    return MOUNTEDSUJ(vnode_mount(vp));
}

bool MOUNTEDSUJ(struct mount *mp)
{
    struct ufsmount *ump = VFSTOUFS(mp);
    return (ump->um_compat->mnt_flag & FREEBSD_MNT_SUJ) != 0;
}
