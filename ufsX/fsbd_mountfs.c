//
//  mountfs.c
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
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


#pragma mark FIXME: fix this....

struct vnode *
mntfs_allocvp(struct mount *mp, struct vnode *ovp)
{
    struct vnode *vp = NULL;
    dev_t dev;

    dev = vnode_specrdev(ovp);

//    getnewvnode("mntfs", mp, &mntfs_vnodeops, &vp);
//    vp->v_type = VCHR;
//    vp->v_data = NULL;
//    vnode_ref(vp);
//    vp->v_rdev = dev;

    return (vp);
}

void
mntfs_freevp(struct vnode *vp)
{
    vnode_rele(vp);
}
