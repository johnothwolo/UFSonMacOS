/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1991, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ufs_inode.c	8.9 (Berkeley) 5/14/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

// #include "opt_quota.h"
// #include "opt_ufs.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/malloc.h>

#include <freebsd/compat/compat.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#ifdef UFS_DIRHASH
#include <ufs/ufs/dir.h>
#include <ufs/ufs/dirhash.h>
#endif

/*
 * Last reference to an inode.  If necessary, write or delete it.
 */
int
ufs_inactive(struct vnop_inactive_args *ap)
    /* {
		struct vnode *a_vp;
	} */
{
	struct vnode *vp = ap->a_vp;
    struct inode *ip = VTOI(vp);
    struct ufsmount *ump = ITOUMP(ip);
    struct vfs_context *context = ap->a_context;
	mode_t mode;
    int locktype;
	int error = 0;
	off_t isize;
	struct mount *mp;

    trace_enter();
    
	mp = NULL;
    locktype = inode_lock_owned(ip);

	/*
	 * Ignore inodes related to stale file handles.
	 */
	if (ip->i_mode == 0)
		goto out;
#ifdef QUOTA
	/*
	 * Before moving off the active list, we must be sure that
	 * any modified quotas have been pushed since these will no
	 * longer be checked once the vnode is on the inactive list.
	 */
	qsyncvp(vp);
#endif
	if ((ip->i_effnlink == 0 && DOINGSOFTDEP(vp)) ||
	    (ip->i_nlink <= 0 && !UFS_RDONLY(ip))) {
	loop:
        if (vn_start_secondary_write(vp, &mp, V_NOWAIT) != 0) {
            /* Cannot delete file while file system is suspended */
            if (VN_IS_DOOMED(vp)) {
                /* Cannot return before file is deleted */
                (void) vn_start_secondary_write(vp, &mp, V_WAIT);
            } else {
                ixlock(ip);
                if ((ump->um_compat->mnt_flag & (MNTK_SUSPEND2 | MNTK_SUSPENDED)) == 0) {
                    iunlock(ip);
                    goto loop;
                }
                /*
                 * Fail to inactivate vnode now and
                 * let ffs_snapshot() clean up after
                 * it has resumed the file system.
                 */
                ip->i_viflag |= VI_NEEDINACT;
                iunlock(ip);
                return (0);
            }
        }
	}
    
	isize = ip->i_size;
	if (I_IS_UFS2(ip))
		isize += ip->i_din2->di_extsize;
	if (ip->i_effnlink <= 0 && isize && !UFS_RDONLY(ip))
		error = UFS_TRUNCATE(vp, (off_t)0, FREEBSD_IO_EXT | FREEBSD_IO_NORMAL, context);
	if (ip->i_nlink <= 0 && ip->i_mode != 0 && !UFS_RDONLY(ip) &&
	    (ip->i_viflag & VI_NEEDINACT) == 0) {
#ifdef QUOTA
		if (!getinoquota(ip))
			(void)chkiq(ip, -1, NOCRED, FORCE);
#endif
#ifdef UFS_EXTATTR
		ufs_extattr_vnode_inactive(vp);
#endif
		/*
		 * Setting the mode to zero needs to wait for the inode
		 * to be written just as does a change to the link count.
		 * So, rather than creating a new entry point to do the
		 * same thing, we just use softdep_change_linkcnt().
		 */
		DIP_SET(ip, i_rdev, 0);
		mode = ip->i_mode;
		ip->i_mode = 0;
		DIP_SET(ip, i_mode, 0);
		UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
		if (DOINGSOFTDEP(vp))
			softdep_change_linkcnt(ip);
		UFS_VFREE(vp, ip->i_number, mode);
	}
	if (ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) {
		if ((ip->i_flag & (IN_CHANGE | IN_UPDATE | IN_MODIFIED)) == 0 &&
		    mp == NULL &&
            vn_start_secondary_write(vp, &mp, V_NOWAIT)) {
            mp = NULL;
            ip->i_flag &= ~IN_ACCESS;
        } else {
            UFS_UPDATE(vp, 0);
		}
	}
out:
	/*
	 * If we are done with the inode, reclaim it
	 * so that it can be reused immediately.
	 */
	if (ip->i_mode == 0 && (ip->i_viflag & VI_NEEDINACT) == 0)
		vnode_recycle(vp);

    return (error);
}

/*
 * Reclaim an inode so that it can be used for other purposes.
 */
int
ufs_reclaim(struct vnop_reclaim_args *ap)
    /* {
		struct vnode *a_vp;
	} */
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
#ifdef QUOTA
	int i;

	for (i = 0; i < MAXQUOTAS; i++) {
		if (ip->i_dquot[i] != NODQUOT) {
			dqrele(vp, ip->i_dquot[i]);
			ip->i_dquot[i] = NODQUOT;
		}
	}
#endif
#ifdef UFS_DIRHASH
	if (ip->i_dirhash != NULL)
		ufsdirhash_free(ip);
#endif

	if (ip->i_flag & IN_LAZYMOD)
		UFS_INODE_SET_FLAG(ip, IN_MODIFIED);
	UFS_UPDATE(vp, 0);
	/*
	 * Remove the inode from its hash chain.
	 */
	ufs_hash_remove(ip);
    vnode_removefsref(vp);
        
	/*
	 * Lock the clearing of v_data so ffs_lock() can inspect it
	 * prior to obtaining the lock.
	 */
	ixlock(ip);
    vnode_clearfsnode(vp);
	iunlock(ip);
	UFS_IFREE(ITOUMP(ip), ip);
	return (0);
}
