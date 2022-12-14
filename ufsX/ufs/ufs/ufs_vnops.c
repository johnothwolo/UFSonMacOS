/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1989, 1993, 1995
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
 *	@(#)ufs_vnops.c	8.27 (Berkeley) 5/27/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

// #include "opt_quota.h"
// #include "opt_suiddir.h"
// #include "opt_ufs.h"
// #include "opt_ffs.h"

#ifndef ISWHITEOUT
#define ISWHITEOUT 0x00020000
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/unistd.h>
#include <sys/dirent.h>
#include <sys/kauth.h>
#include <sys/lockf.h>
#include <sys/conf.h>
#include <sys/ubc.h>

#include <vfs/vfs_support.h>
#include <sys/file.h>		/* XXX */

#include <freebsd/compat/compat.h>

#include <ufs/ufsX_vnops.h>
#include <ufs/ufs/acl.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ffs/fs.h>
#ifdef UFS_DIRHASH
#include <ufs/ufs/dirhash.h>
#endif

#ifndef SAVESTART
#define SAVESTART       0x00001000 /* save starting directory */
#endif

/* ioctls to support SEEK_HOLE SEEK_DATA */
#define FSIOC_FIOSEEKHOLE                                         _IOWR('A', 16, off_t)
#define FSCTL_FIOSEEKHOLE                                         IOCBASECMD(FSIOC_FIOSEEKHOLE)
#define FSIOC_FIOSEEKDATA                                         _IOWR('A', 17, off_t)
#define FSCTL_FIOSEEKDATA                                         IOCBASECMD(FSIOC_FIOSEEKDATA)

#ifdef QUOTA
FEATURE(ufs_quota, "UFS disk quotas support");
FEATURE(ufs_quota64, "64bit UFS disk quotas support");
#endif

#ifdef SUIDDIR
FEATURE(suiddir,
    "Give all new files in directory the same ownership as the directory");
#endif

// prototypes
static int ufs_chmod(struct vnode *, int, struct ucred *, struct vfs_context *);
static int ufs_chown(struct vnode *, uid_t, gid_t, struct ucred *, struct vfs_context *);
static int ufs_makeinode(int mode, struct vnode *, struct vnode **,
                         struct componentname *, const char *, struct vfs_context *);

SYSCTL_NODE(_vfs, OID_AUTO, ufs, CTLFLAG_RD, 0, "UFS filesystem");

/*
 * A virgin directory (no blushing please).
 */
static struct dirtemplate mastertemplate = {
	0, 12, DT_DIR, 1, ".",
	0, DIRBLKSIZ - 12, DT_DIR, 2, ".."
};
static struct odirtemplate omastertemplate = {
	0, 12, 1, ".",
	0, DIRBLKSIZ - 12, 2, ".."
};

static void
ufs_itimes_locked(struct inode *ip)
{
    struct vnode *vp;
    struct timespec ts;
    struct bsdmount *bmp;

    assert(inode_lock_owned(ip) == UFS_LOCK_EXCLUSIVE);
    
	vp = ITOV(ip);
    bmp = ITOUMP(ip)->um_compat;
    
	if (UFS_RDONLY(ip))
		goto out;
	if ((ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_UPDATE)) == 0)
		return;

	if ((vnode_vtype(vp) == VBLK || vnode_vtype(vp) == VCHR) && !DOINGSOFTDEP(vp))
		UFS_INODE_SET_FLAG(ip, IN_LAZYMOD);
	else if (((bmp->mnt_kern_flag & (MNTK_SUSPENDED | MNTK_SUSPEND)) == 0) ||
             (ip->i_flag & (IN_CHANGE | IN_UPDATE)))
		UFS_INODE_SET_FLAG(ip, IN_MODIFIED);
	else if (ip->i_flag & IN_ACCESS)
		UFS_INODE_SET_FLAG(ip, IN_LAZYACCESS);
	getnanotime(&ts);
	if (ip->i_flag & IN_ACCESS) {
		DIP_SET(ip, i_atime, (int)ts.tv_sec);
		DIP_SET(ip, i_atimensec, (int)ts.tv_nsec);
	}
	if (ip->i_flag & IN_UPDATE) {
		DIP_SET(ip, i_mtime, (int)ts.tv_sec);
		DIP_SET(ip, i_mtimensec, (int)ts.tv_nsec);
	}
	if (ip->i_flag & IN_CHANGE) {
		DIP_SET(ip, i_ctime, (int)ts.tv_sec);
		DIP_SET(ip, i_ctimensec, (int)ts.tv_nsec);
		DIP_SET(ip, i_modrev, DIP(ip, i_modrev) + 1);
	}

 out:
	ip->i_flag &= ~(IN_ACCESS | IN_CHANGE | IN_UPDATE);
}

void
ufs_itimes(struct vnode *vp)
{
    struct inode *ip = VTOI(vp);
    
	ixlock(ip);
	ufs_itimes_locked(ip);
	iunlock(ip);
}

/*
 * Create a regular file
 */
int
ufs_create(struct vnop_create_args *ap)
    /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
        struct vfs_context *a_context;
	} */
{
	int error;

	error = ufs_makeinode(MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
                      ap->a_dvp, ap->a_vpp, ap->a_cnp, "ufs_create", ap->a_context);
	if (error != 0)
		return (error);
	if ((ap->a_cnp->cn_flags & MAKEENTRY) != 0)
		cache_enter(ap->a_dvp, *ap->a_vpp, ap->a_cnp);
	return (0);
}

errno_t VNOP_CREATE(vnode_t dvp, vnode_t * vpp, struct componentname * cnp,
                    struct vnode_attr * vap, vfs_context_t ctx)
{
    struct vnop_create_args args = {
        .a_desc = &vnop_create_desc,
        .a_dvp = dvp,
        .a_vpp = vpp,
        .a_cnp = cnp,
        .a_vap = vap,
        .a_context = ctx,
    };
    return ufs_create(&args);
}

/*
 * Mknod vnode call
 */
/* ARGSUSED */
int
ufs_mknod(
	struct vnop_mknod_args /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
        struct vfs_context *a_context;
	} */ *ap)
{
	struct vnode_attr *vap = ap->a_vap;
	struct vnode **vpp = ap->a_vpp;
	struct inode *ip;
	int error;

	error = ufs_makeinode(MAKEIMODE(vap->va_type, vap->va_mode),
                          ap->a_dvp, vpp, ap->a_cnp, "ufs_mknod", ap->a_context);
	if (error)
		return (error);
    
	ip = VTOI(*vpp);
	UFS_INODE_SET_FLAG(ip, IN_ACCESS | IN_CHANGE | IN_UPDATE);
    
	if (vap->va_rdev != VNOVAL) {
		/*
		 * Want to be able to use this to make badblock
		 * inodes, so don't truncate the dev number.
		 */
		DIP_SET(ip, i_rdev, vap->va_rdev);
	}
	/*
	 * Remove inode, then reload it through VFS_VGET so it is
	 * checked to see if it is an alias of an existing entry in
	 * the inode cache.  XXX I don't believe this is necessary now.
	 */
#if not_necessary
    ino_t ino = ip->i_number;	/* Save this before vgone() invalidates ip. */
	vnode_recycle(*vpp);
	vnode_put(*vpp);
#error Can't pass NULL for 3rd argument
	error = VFS_VGET(vnode_mount(ap->a_dvp), ino, NULL, vpp, ap->a_context);
	if (error) {
		*vpp = NULL;
		return (error);
	}
#endif
	return (0);
}

/*
 * Open called.
 */
/* ARGSUSED */
int
ufs_open(struct vnop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip;

	if (vnode_vtype(vp) == VCHR || vnode_vtype(vp) == VBLK)
		trace_return (EOPNOTSUPP);

	ip = VTOI(vp);
    
	if ((ip->i_flags & APPEND) && (ap->a_mode & (FWRITE | O_APPEND)) == FWRITE)
		trace_return (EPERM);

	trace_return (0);
}

/*
 * Close called.
 *
 * Update the times on the inode.
 */
/* ARGSUSED */
int
ufs_vnop_close(struct vnop_close_args *ap)
    /* {
		struct vnode *a_vp;
		int  a_fflag;
		struct ucred *a_cred;
		struct vfs_context *a_td;
	} */
{
	struct vnode *vp = ap->a_vp;

	if (vnode_isinuse(vp, 1))
		ufs_itimes(vp);
	trace_return (0);
}

/* ARGSUSED */
int
ufs_getattr(struct vnop_getattr_args *ap)
    /* {
		struct vnode *a_vp;
		struct vnode_attr *a_vap;
		struct ucred *a_cred;
	} */
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct vnode_attr *vap = ap->a_vap;
    struct vfsstatfs *statfs = vfs_statfs(vnode_mount(vp));

	ixlock(ip);
	ufs_itimes_locked(ip);
	if (I_IS_UFS1(ip)) {
		vap->va_access_time.tv_sec = ip->i_din1->di_atime;
		vap->va_access_time.tv_nsec = ip->i_din1->di_atimensec;
	} else {
		vap->va_access_time.tv_sec = ip->i_din2->di_atime;
		vap->va_access_time.tv_nsec = ip->i_din2->di_atimensec;
	}
	iunlock(ip);
	/*
	 * Copy from inode table
	 */
    vap->va_fsid = (int)ip->i_dev;
	vap->va_fileid = ip->i_number;
	vap->va_mode = ip->i_mode & ~IFMT;
	vap->va_nlink = ip->i_effnlink;
	vap->va_uid = ip->i_uid;
	vap->va_gid = ip->i_gid;
	if (I_IS_UFS1(ip)) {
		vap->va_rdev = ip->i_din1->di_rdev;
		vap->va_data_size = ip->i_din1->di_size;
		vap->va_modify_time.tv_sec = ip->i_din1->di_mtime;
		vap->va_modify_time.tv_nsec = ip->i_din1->di_mtimensec;
		vap->va_create_time.tv_sec = ip->i_din1->di_ctime;
		vap->va_create_time.tv_nsec = ip->i_din1->di_ctimensec;
		vap->va_data_alloc = dbtob((u_quad_t)ip->i_din1->di_blocks, DEV_BSIZE);
		vap->va_filerev = ip->i_din1->di_modrev;
	} else {
		vap->va_rdev = (unsigned)ip->i_din2->di_rdev;
		vap->va_data_size = ip->i_din2->di_size;
		vap->va_modify_time.tv_sec = ip->i_din2->di_mtime;
		vap->va_modify_time.tv_nsec = ip->i_din2->di_mtimensec;
		vap->va_change_time.tv_sec = ip->i_din2->di_ctime;
		vap->va_change_time.tv_nsec = ip->i_din2->di_ctimensec;
		vap->va_create_time.tv_sec = ip->i_din2->di_birthtime;
		vap->va_create_time.tv_nsec = ip->i_din2->di_birthnsec;
		vap->va_data_alloc = dbtob((u_quad_t)ip->i_din2->di_blocks, DEV_BSIZE);
		vap->va_filerev = ip->i_din2->di_modrev;
	}
	vap->va_flags = ip->i_flags;
	vap->va_gen = (unsigned)ip->i_gen;
    vap->va_iosize = (unsigned)statfs->f_iosize;
	vap->va_type = IFTOVT(ip->i_mode);
	trace_return (0);
}

/*
 * Set attribute vnode op. called from several syscalls
 */
int
ufs_setattr(struct vnop_setattr_args *ap)
    /* {
		struct vnode *a_vp;
		struct vnode_attr *a_vap;
		struct ucred *a_cred;
	} */
{
	struct vnode_attr *vap = ap->a_vap;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
    struct vfs_context *context = ap->a_context;
	struct ucred *cred= vfs_context_ucred(context);
	int error;

	/*
	 * Check for unsettable attributes.
	 */
	if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
	    (vap->va_iosize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
	    ((int)vap->va_data_size != VNOVAL) || (vap->va_gen != VNOVAL)) {
		return (EINVAL);
	}
    
    VATTR_SET_SUPPORTED(vap, va_flags);
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		if ((vap->va_flags & ~(SF_APPEND | SF_ARCHIVED | SF_IMMUTABLE |
		    SF_NOUNLINK | SF_SNAPSHOT | UF_APPEND | UF_COMPRESSED |
		    UF_HIDDEN | UF_IMMUTABLE | UF_NODUMP | UF_SETTABLE | UF_OPAQUE)) != 0)
            trace_return (EOPNOTSUPP);
		if (vfs_isrdonly(vnode_mount(vp)))
			trace_return (EROFS);
		/*
		 * Callers may only modify the file flags on objects they
		 * have VADMIN rights for.
		 */
        error = vnode_authorize(vp, NULLVP, KAUTH_VNODE_TAKE_OWNERSHIP, context);
		if (error)
			return (error);
		/*
		 * Unprivileged processes are not permitted to unset system
		 * flags, or modify flags if any system flags are set.
		 * Privileged non-jail processes may not modify system flags
		 * if securelevel > 0 and any existing system flags are set.
		 * Privileged jail processes behave like privileged non-jail
		 * processes if the PR_ALLOW_CHFLAGS permission bit is set;
		 * otherwise, they behave like unprivileged processes.
		 */
        if (cred->cr_posix.cr_uid == ip->i_uid && groupmember(ip->i_gid, cred)) {

			/* The snapshot flag cannot be toggled. */
			if ((vap->va_flags ^ ip->i_flags) & SF_SNAPSHOT)
				return (EPERM);
		} else {
			if (ip->i_flags &
			    (SF_NOUNLINK | SF_IMMUTABLE | SF_APPEND) ||
			    ((vap->va_flags ^ ip->i_flags) & SF_SETTABLE))
				return (EPERM);
		}
		ip->i_flags = vap->va_flags;
		DIP_SET(ip, i_flags, vap->va_flags);
		UFS_INODE_SET_FLAG(ip, IN_CHANGE);
		error = UFS_UPDATE(vp, 0);
		if (ip->i_flags & (IMMUTABLE | APPEND))
			return (error);
	}
	/*
	 * If immutable or append, no one can change any of its attributes
	 * except the ones already handled (in some cases, file flags
	 * including the immutability flags themselves for the superuser).
	 */
	if (ip->i_flags & (IMMUTABLE | APPEND))
		return (EPERM);
	/*
	 * Go through the fields and update iff not VNOVAL.
	 */
    VATTR_SET_SUPPORTED(vap, va_gid);
    VATTR_SET_SUPPORTED(vap, va_uid);
	if ((vap->va_uid != (uid_t)VNOVAL && VATTR_IS_ACTIVE(vap, va_gid)) ||
        (vap->va_gid != (gid_t)VNOVAL && VATTR_IS_ACTIVE(vap, va_uid))) {
		if (vfs_isrdonly(vnode_mount(vp)))
			return (EROFS);
		if ((error = ufs_chown(vp, vap->va_uid, vap->va_gid, cred, context)) != 0)
			return (error);
	}
    
    VATTR_SET_SUPPORTED(vap, va_data_size);
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		/*
		 * XXX most of the following special cases should be in
		 * callers instead of in N filesystems.  The VDIR check
		 * mostly already is.
		 */
		switch (vnode_vtype(vp)) {
		case VDIR:
			return (EISDIR);
		case VLNK:
		case VREG:
			/*
			 * Truncation should have an effect in these cases.
			 * Disallow it if the filesystem is read-only or
			 * the file is being snapshotted.
			 */
			if (vfs_isrdonly(vnode_mount(vp)))
				return (EROFS);
			if ((ip->i_flags & SF_SNAPSHOT) != 0)
				return (EPERM);
			break;
		default:
			/*
			 * According to POSIX, the result is unspecified
			 * for file types other than regular files,
			 * directories and shared memory objects.  We
			 * don't support shared memory objects in the file
			 * system, and have dubious support for truncating
			 * symlinks.  Just ignore the request in other cases.
			 */
			return (0);
		}
        // because we don't support VA_SYNC, we always synronously truncate here
		if ((error = UFS_TRUNCATE(vp, vap->va_data_size, FREEBSD_IO_NORMAL | IO_SYNC , context)) != 0)
			return (error);
	}
    // ufs doesn't support backup time
    VATTR_CLEAR_SUPPORTED(vap, va_backup_time);
    VATTR_SET_SUPPORTED(vap, va_create_time);
    VATTR_SET_SUPPORTED(vap, va_access_time);
    VATTR_SET_SUPPORTED(vap, va_modify_time);
    VATTR_SET_SUPPORTED(vap, va_change_time);
	if (VATTR_IS_ACTIVE(vap, va_access_time) ||
	    VATTR_IS_ACTIVE(vap, va_modify_time) ||
	    VATTR_IS_ACTIVE(vap, va_create_time)) {
		if (vfs_isrdonly(vnode_mount(vp)))
			return (EROFS);
		if ((ip->i_flags & SF_SNAPSHOT) != 0)
			return (EPERM);
        
		UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_MODIFIED);
        
		if (vap->va_access_time.tv_sec != VNOVAL) {
			ip->i_flag &= ~IN_ACCESS;
			DIP_SET(ip, i_atime, (int)vap->va_access_time.tv_sec);
			DIP_SET(ip, i_atimensec, (int)vap->va_access_time.tv_nsec);
		}
		if (vap->va_modify_time.tv_sec != VNOVAL) {
			ip->i_flag &= ~IN_UPDATE;
			DIP_SET(ip, i_mtime, (int)vap->va_modify_time.tv_sec);
			DIP_SET(ip, i_mtimensec, (int)vap->va_modify_time.tv_nsec);
		}
		if (vap->va_create_time.tv_sec != VNOVAL && I_IS_UFS2(ip)) {
			ip->i_din2->di_birthtime = vap->va_create_time.tv_sec;
			ip->i_din2->di_birthnsec = (int)vap->va_create_time.tv_nsec;
		}
		error = UFS_UPDATE(vp, 0);
		if (error)
			return (error);
	}
	error = 0;
	if (vap->va_mode != (mode_t)VNOVAL) {
		if (vfs_isrdonly(vnode_mount(vp)))
			return (EROFS);
		if ((ip->i_flags & SF_SNAPSHOT) != 0 && (vap->va_mode &
		   (S_IXUSR | S_IWUSR | S_IXGRP | S_IWGRP | S_IXOTH | S_IWOTH)))
			return (EPERM);
		error = ufs_chmod(vp, (int)vap->va_mode, cred, context);
	}
	trace_return (error);
}

#ifdef UFS_ACL
static int
ufs_update_nfs4_acl_after_mode_change(struct vnode *vp, int mode,
    int file_owner_id, struct ucred *cred, vfs_context_t context)
{
	int error;
	struct acl *aclp;

	aclp = acl_alloc(M_WAITOK);
	error = ufs_getacl_nfs4_internal(vp, aclp, td);
	/*
	 * We don't have to handle EOPNOTSUPP here, as the filesystem claims
	 * it supports ACLs.
	 */
	if (error)
		goto out;

	acl_nfs4_sync_acl_from_mode(aclp, mode, file_owner_id);
	error = ufs_setacl_nfs4_internal(vp, aclp, td);

out:
	acl_free(aclp);
	return (error);
}
#endif /* UFS_ACL */

int
ufs_mmap(struct vnop_mmap_args *ap)
    /* {
		struct vnode *a_vp;
	} */
{
	struct vnode *vp;
	struct inode *ip;
	struct mount *mp;

	vp = ap->a_vp;
	ip = VTOI(vp);
	mp = vnode_mount(vp);

	if ((vfs_flags(mp) & (MNT_NOATIME | MNT_RDONLY)) == 0)
		UFS_INODE_SET_FLAG_SHARED(ip, IN_ACCESS);
	/*
	 * XXXKIB No UFS_UPDATE(ap->a_vp, 0) there.
	 */
	trace_return (0);
}

/*
 * Change the mode on a file.
 * Inode must be locked before calling.
 */
int
ufs_chmod(struct vnode *vp, int mode, struct ucred *cred, vfs_context_t context)
{
	struct inode *ip = VTOI(vp);
	int newmode, error;

	/*
	 * To modify the permissions on a file, must possess VADMIN
	 * for that file.
	 */
	if ((error = vnode_authorize(vp, NULLVP, KAUTH_VNODE_TAKE_OWNERSHIP, context)))
		trace_return (error);
	/*
	 * Privileged processes may set the sticky bit on non-directories,
	 * as well as set the setgid bit on a file with a group that the
	 * process is not a member of.  Both of these are allowed in
	 * jail(8).
	 */
	if (vnode_vtype(vp) != VDIR && (mode & S_ISTXT) &&
        !kauth_cred_issuser(cred)) {
        trace_return (EFTYPE);
	}
    
    if (!groupmember(ip->i_gid, cred) && (mode & ISGID) &&
        !kauth_cred_issuser(cred)){
        trace_return (EPERM);
	}

	/*
	 * Deny setting setuid if we are not the file owner.
	 */
	if ((mode & ISUID) && ip->i_uid != cred->cr_posix.cr_uid &&
        !kauth_cred_issuser(cred)) {
        trace_return (EPERM);
	}

	newmode = ip->i_mode & ~ALLPERMS;
	newmode |= (mode & ALLPERMS);
	UFS_INODE_SET_MODE(ip, newmode);
	DIP_SET(ip, i_mode, ip->i_mode);
	UFS_INODE_SET_FLAG(ip, IN_CHANGE);
#ifdef UFS_ACL
	if ((vfs_flags(vnode_mount(vp)) & FREEBSD_MNT_NFS4ACLS) != 0)
		error = ufs_update_nfs4_acl_after_mode_change(vp, mode, ip->i_uid, cred, td);
#endif
	if (error == 0 && (ip->i_flag & IN_CHANGE) != 0)
		error = UFS_UPDATE(vp, 0);

	trace_return (error);
}

/*
 * Perform chown operation on inode ip;
 * inode must be locked prior to call.
 */
int
ufs_chown(struct vnode *vp, uid_t uid, gid_t gid, struct ucred *cred, vfs_context_t context)
{
	struct inode *ip = VTOI(vp);
	uid_t ouid;
	gid_t ogid;
	int error = 0;
#ifdef QUOTA
	int i;
	ufs2_daddr_t change;
#endif

	if (uid == (uid_t)VNOVAL)
		uid = ip->i_uid;
	if (gid == (gid_t)VNOVAL)
		gid = ip->i_gid;
	/*
	 * To modify the ownership of a file, must possess VADMIN for that
	 * file.
	 */
	if ((error = vnode_authorize(vp, NULLVP, KAUTH_VNODE_TAKE_OWNERSHIP, context)))
		trace_return (error);
	/*
	 * To change the owner of a file, or change the group of a file to a
	 * group of which we are not a member, the caller must have
	 * privilege.
	 */
	if (((uid != ip->i_uid && uid != cred->cr_posix.cr_uid) || 
	    (gid != ip->i_gid && !groupmember(gid, cred))) &&
	    !kauth_cred_issuser(cred))
        trace_return (EPERM);
	ogid = ip->i_gid;
	ouid = ip->i_uid;
#ifdef QUOTA
	if ((error = getinoquota(ip)) != 0)
		return (error);
	if (ouid == uid) {
		dqrele(vp, ip->i_dquot[USRQUOTA]);
		ip->i_dquot[USRQUOTA] = NODQUOT;
	}
	if (ogid == gid) {
		dqrele(vp, ip->i_dquot[GRPQUOTA]);
		ip->i_dquot[GRPQUOTA] = NODQUOT;
	}
	change = DIP(ip, i_blocks);
	(void) chkdq(ip, -change, cred, CHOWN|FORCE);
	(void) chkiq(ip, -1, cred, CHOWN|FORCE);
	for (i = 0; i < MAXQUOTAS; i++) {
		dqrele(vp, ip->i_dquot[i]);
		ip->i_dquot[i] = NODQUOT;
	}
#endif
	ip->i_gid = gid;
	DIP_SET(ip, i_gid, gid);
	ip->i_uid = uid;
	DIP_SET(ip, i_uid, uid);
#ifdef QUOTA
	if ((error = getinoquota(ip)) == 0) {
		if (ouid == uid) {
			dqrele(vp, ip->i_dquot[USRQUOTA]);
			ip->i_dquot[USRQUOTA] = NODQUOT;
		}
		if (ogid == gid) {
			dqrele(vp, ip->i_dquot[GRPQUOTA]);
			ip->i_dquot[GRPQUOTA] = NODQUOT;
		}
		if ((error = chkdq(ip, change, cred, CHOWN)) == 0) {
			if ((error = chkiq(ip, 1, cred, CHOWN)) == 0)
				goto good;
			else
				(void) chkdq(ip, -change, cred, CHOWN|FORCE);
		}
		for (i = 0; i < MAXQUOTAS; i++) {
			dqrele(vp, ip->i_dquot[i]);
			ip->i_dquot[i] = NODQUOT;
		}
	}
	ip->i_gid = ogid;
	DIP_SET(ip, i_gid, ogid);
	ip->i_uid = ouid;
	DIP_SET(ip, i_uid, ouid);
	if (getinoquota(ip) == 0) {
		if (ouid == uid) {
			dqrele(vp, ip->i_dquot[USRQUOTA]);
			ip->i_dquot[USRQUOTA] = NODQUOT;
		}
		if (ogid == gid) {
			dqrele(vp, ip->i_dquot[GRPQUOTA]);
			ip->i_dquot[GRPQUOTA] = NODQUOT;
		}
		(void) chkdq(ip, change, cred, FORCE|CHOWN);
		(void) chkiq(ip, 1, cred, FORCE|CHOWN);
		(void) getinoquota(ip);
	}
	trace_return (error);
good:
	if (getinoquota(ip))
		panic("ufs_chown: lost quota");
#endif /* QUOTA */
	UFS_INODE_SET_FLAG(ip, IN_CHANGE);
	if ((ip->i_mode & (ISUID | ISGID)) && (ouid != uid || ogid != gid)) {
		if (kauth_cred_issuser(cred)) {
			UFS_INODE_SET_MODE(ip, ip->i_mode & ~(ISUID | ISGID));
			DIP_SET(ip, i_mode, ip->i_mode);
		}
	}
	error = UFS_UPDATE(vp, 0);
	trace_return (error);
}

int
ufs_remove(struct vnop_remove_args *ap)
    /* {
		struct vnode *a_dvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
	} */
{
	struct inode *ip;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
    vfs_context_t context = ap->a_context;
	int error;

	ip = VTOI(vp);
	if ((ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND)) ||
	    (VTOI(dvp)->i_flags & APPEND))
		return (EPERM);
	if (DOINGSOFTDEP(dvp)) {
		error = softdep_prelink(dvp, vp, true);
		if (error != 0) {
			MPASS(error == ERECYCLE);
			trace_return (error);
		}
	}

	error = ufs_dirremove(dvp, ip, ap->a_cnp->cn_flags, 0);

	if ((ip->i_flags & SF_SNAPSHOT) != 0) {
		/*
		 * Avoid deadlock where another thread is trying to
		 * update the inodeblock for dvp and is waiting on
		 * snaplk.  Temporary unlock the vnode lock for the
		 * unlinked file and sync the directory.  This should
		 * allow vnode_put() of the directory to not block later on
		 * while holding the snapshot vnode locked, assuming
		 * that the directory hasn't been unlinked too.
		 */
		VNOP_UNLOCK(vp);
		(void) VNOP_FSYNC(dvp, MNT_WAIT, context);
		vn_lock(vp, UFS_LOCK_EXCLUSIVE | UFS_LOCK_RETRY);
	}
	trace_return (error);
}

static void
print_bad_link_count(const char *funcname, struct vnode *dvp)
{
	struct inode *dip;

	dip = VTOI(dvp);
	log_debug("%s: Bad link count %d on parent inode %lld in file system %s\n",
	    funcname, dip->i_effnlink, (intmax_t)dip->i_number,
	    vfs_statfs(vnode_mount(dvp))->f_mntonname);
}

/*
 * link vnode call
 */
int
ufs_link(struct vnop_link_args *ap)
    /* {
		struct vnode *a_tdvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
        vfs_context_t a_context;
	} */
{
	struct vnode *vp = ap->a_vp;
	struct vnode *tdvp = ap->a_tdvp;
	struct componentname *cnp = ap->a_cnp;
    vfs_context_t context = ap->a_context;
	struct inode *ip;
	struct direct newdir;
	int error;

#ifdef INVARIANTS
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("ufs_link: no name");
#endif

	if (DOINGSOFTDEP(tdvp)) {
		error = softdep_prelink(tdvp, vp, true);
		if (error != 0) {
			MPASS(error == ERECYCLE);
			return (error);
		}
	}

	if (VTOI(tdvp)->i_effnlink < 2) {
		print_bad_link_count("ufs_link", tdvp);
		error = EINVAL;
		goto out;
	}
	ip = VTOI(vp);
	if (ip->i_nlink >= UFS_LINK_MAX) {
		error = EMLINK;
		goto out;
	}
	/*
	 * The file may have been removed after namei droped the original
	 * lock.
	 */
	if (ip->i_effnlink == 0) {
		error = ENOENT;
		goto out;
	}
	if (ip->i_flags & (IMMUTABLE | APPEND)) {
		error = EPERM;
		goto out;
	}

	ip->i_effnlink++;
	ip->i_nlink++;
	DIP_SET(ip, i_nlink, ip->i_nlink);
	UFS_INODE_SET_FLAG(ip, IN_CHANGE);
	if (DOINGSOFTDEP(vp))
		softdep_setup_link(VTOI(tdvp), ip);
	error = UFS_UPDATE(vp, !DOINGSOFTDEP(vp) && !DOINGASYNC(vp));
	if (!error) {
		ufs_makedirentry(ip, cnp, &newdir);
		error = ufs_direnter(tdvp, vp, &newdir, cnp, NULL, 0, context);
	}

	if (error) {
		ip->i_effnlink--;
		ip->i_nlink--;
		DIP_SET(ip, i_nlink, ip->i_nlink);
		UFS_INODE_SET_FLAG(ip, IN_CHANGE);
		if (DOINGSOFTDEP(vp))
			softdep_revert_link(VTOI(tdvp), ip);
	}
out:
	return (error);
}

/*
 * whiteout vnode call
 */
int
ufs_whiteout(struct vnop_whiteout_args *ap)
    /* {
		struct vnode *a_dvp;
		struct componentname *a_cnp;
		int a_flags;
        vfs_context_t a_context;
	} */
{
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
    vfs_context_t context = ap->a_context;
	struct direct newdir;
	int error = 0;

	if (DOINGSOFTDEP(dvp) && (ap->a_flags == CREATE ||
	    ap->a_flags == DELETE)) {
		error = softdep_prelink(dvp, NULL, true);
		if (error != 0) {
			MPASS(error == ERECYCLE);
			return (error);
		}
	}

	switch (ap->a_flags) {
	case LOOKUP:
		/* 4.4 format directories support whiteout operations */
		if (vfs_maxsymlen(vnode_mount(dvp)) > 0)
			return (0);
		return (EOPNOTSUPP);

	case CREATE:
		/* create a new directory whiteout */
#ifdef INVARIANTS
		if ((cnp->cn_flags & SAVENAME) == 0)
			panic("ufs_whiteout: missing name");
		if (vfs_maxsymlen(vnode_mount(dvp)) <= 0)
			panic("ufs_whiteout: old format filesystem");
#endif

		newdir.d_ino = UFS_WINO;
		newdir.d_namlen = cnp->cn_namelen;
		bcopy(cnp->cn_nameptr, newdir.d_name, (unsigned)cnp->cn_namelen + 1);
		newdir.d_type = DT_WHT;
		error = ufs_direnter(dvp, NULL, &newdir, cnp, NULL, 0, context);
		break;

	case DELETE:
		/* remove an existing directory whiteout */
#ifdef INVARIANTS
		if (vfs_maxsymlen(vnode_mount(dvp)) <= 0)
			panic("ufs_whiteout: old format filesystem");
#endif

		cnp->cn_flags &= ~DOWHITEOUT;
		error = ufs_dirremove(dvp, NULL, cnp->cn_flags, 0);
		break;
	default:
		panic("ufs_whiteout: unknown op");
	}
	return (error);
}

static volatile int rename_restarts;
SYSCTL_INT(_vfs_ufs, OID_AUTO, rename_restarts, CTLFLAG_RD,
    __DEVOLATILE(int *, &rename_restarts), 0,
    "Times rename had to restart due to lock contention");

/*
 * Rename system call.
 * 	rename("foo", "bar");
 * is essentially
 *	unlink("bar");
 *	link("foo", "bar");
 *	unlink("foo");
 * but ``atomically''.  Can't do full commit without saving state in the
 * inode on disk which isn't feasible at this time.  Best we can do is
 * always guarantee the target exists.
 *
 * Basic algorithm is:
 *
 * 1) Bump link count on source while we're linking it to the
 *    target.  This also ensure the inode won't be deleted out
 *    from underneath us while we work (it may be truncated by
 *    a concurrent `trunc' or `open' for creation).
 * 2) Link source to destination.  If destination already exists,
 *    delete it first.
 * 3) Unlink source reference to inode if still around. If a
 *    directory was moved and the parent of the destination
 *    is different from the source, patch the ".." entry in the
 *    directory.
 */
int
ufs_rename(struct vnop_rename_args *ap)
    /* {
		struct vnode *a_fdvp;
		struct vnode *a_fvp;
		struct componentname *a_fcnp;
		struct vnode *a_tdvp;
		struct vnode *a_tvp;
		struct componentname *a_tcnp;
	} */
{
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct vnode *nvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	vfs_context_t context = ap->a_context;
    struct vfs_vget_args vargs = {0};
	struct inode *fip, *tip, *tdp, *fdp;
	struct direct newdir;
	off_t endoff;
	int doingdirectory, newparent;
    int error = 0, error1 = 0;
	struct mount *mp;
	ino_t ino;
	bool want_seqc_end;

	want_seqc_end = false;

#ifdef INVARIANTS
	if ((tcnp->cn_flags & HASBUF) == 0 ||
	    (fcnp->cn_flags & HASBUF) == 0)
		panic("ufs_rename: no name");
#endif
	endoff = 0;
	mp = vnode_mount(tdvp);
    
	/*
	 * Check for cross-device rename.
	 */
	if ((vnode_mount(fvp) != vnode_mount(tdvp)) ||
	    (tvp && (vnode_mount(fvp) != vnode_mount(tvp)))) {
		error = EXDEV;
		mp = NULL;
		goto releout;
	}
relock:
	/* 
	 * We need to acquire 2 to 4 locks depending on whether tvp is NULL
	 * and fdvp and tdvp are the same directory.  Subsequently we need
	 * to double-check all paths and in the directory rename case we
	 * need to verify that we are not creating a directory loop.  To
	 * handle this we acquire all but fdvp using non-blocking
	 * acquisitions.  If we fail to acquire any lock in the path we will
	 * drop all held locks, acquire the new lock in a blocking fashion,
	 * and then release it and restart the rename.  This acquire/release
	 * step ensures that we do not spin on a lock waiting for release.
	 */
    ixlock(VTOI(fdvp));
    ixlock(VTOI(tdvp));
	/*
	 * Re-resolve fvp to be certain it still exists and fetch the
	 * correct vnode.
	 */
	error = ufs_lookup_ino(fdvp, NULL, fcnp, &ino, context);
	if (error) {
		iunlock(VTOI(fdvp));
		iunlock(VTOI(tdvp));
		goto releout;
	}
	error = VFS_VGET(mp, ino, &vargs, &nvp, context);
	if (error) {
		iunlock(VTOI(fdvp));
		iunlock(VTOI(tdvp));
		if (error != EBUSY)
			goto releout;
		error = VFS_VGET(mp, ino, &vargs, &nvp, context);
		if (error != 0)
			goto releout;
		vnode_rele(fvp);
		fvp = nvp;
		OSAddAtomic(1, &rename_restarts);
		goto relock;
	}
	vnode_rele(fvp);
	fvp = nvp;
    ixlock(VTOI(fvp));
	/*
	 * Re-resolve tvp and acquire the vnode lock if present.
	 */
	error = ufs_lookup_ino(tdvp, NULL, tcnp, &ino, context);
	if (error != 0 && error != EJUSTRETURN) {
		iunlock(VTOI(fdvp));
		iunlock(VTOI(tdvp));
		iunlock(VTOI(fvp));
		goto releout;
	}
	/*
	 * If tvp disappeared we just carry on.
	 */
	if (error == EJUSTRETURN && tvp != NULL) {
		vnode_rele(tvp);
		tvp = NULL;
	}
	/*
	 * Get the tvp ino if the lookup succeeded.  We may have to restart
	 * if the non-blocking acquire fails.
	 */
	if (error == 0) {
		nvp = NULL;
		error = VFS_VGET(mp, ino, &vargs, &nvp, context);
		if (tvp)
			vnode_rele(tvp);
		tvp = nvp;
		if (error) {
			iunlock(VTOI(fdvp));
			iunlock(VTOI(tdvp));
			iunlock(VTOI(fvp));
			if (error != EBUSY)
				goto releout;
			error = VFS_VGET(mp, ino, &vargs, &nvp, context);
			if (error != 0)
				goto releout;
			vnode_put(nvp);
			OSAddAtomic(1, &rename_restarts);
			goto relock;
		}
	}

	if (DOINGSOFTDEP(fdvp)) {
		error = softdep_prerename(fdvp, fvp, tdvp, tvp);
		if (error != 0) {
			if (error == ERECYCLE) {
				OSAddAtomic(1, &rename_restarts);
				goto relock;
			}
			goto releout;
		}
	}

	fdp = VTOI(fdvp);
	fip = VTOI(fvp);
	tdp = VTOI(tdvp);
	tip = NULL;
	if (tvp)
		tip = VTOI(tvp);
	if (tvp && ((VTOI(tvp)->i_flags & (NOUNLINK | IMMUTABLE | APPEND)) ||
	    (VTOI(tdvp)->i_flags & APPEND))) {
		error = EPERM;
		goto unlockout;
	}
	/*
	 * Renaming a file to itself has no effect.  The upper layers should
	 * not call us in that case.  However, things could change after
	 * we drop the locks above.
	 */
	if (fvp == tvp) {
		error = 0;
		goto unlockout;
	}
	doingdirectory = 0;
	newparent = 0;
	ino = fip->i_number;
	if (fip->i_nlink >= UFS_LINK_MAX) {
		error = EMLINK;
		goto unlockout;
	}
	if ((fip->i_flags & (NOUNLINK | IMMUTABLE | APPEND))
	    || (fdp->i_flags & APPEND)) {
		error = EPERM;
		goto unlockout;
	}
	if ((fip->i_mode & IFMT) == IFDIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
		    fdp == fip ||
		    (fcnp->cn_flags | tcnp->cn_flags) & ISDOTDOT) {
			error = EINVAL;
			goto unlockout;
		}
		if (fdp->i_number != tdp->i_number)
			newparent = tdp->i_number;
		doingdirectory = 1;
	}
	if ((vnode_vtype(fvp) == VDIR && vnode_ismountedon(fvp) != 0) ||
	    (tvp != NULL && vnode_vtype(tvp) == VDIR &&
	     vnode_ismountedon(tvp) != 0)) {
		error = EXDEV;
		goto unlockout;
	}

	/*
	 * If ".." must be changed (ie the directory gets a new
	 * parent) then the source directory must not be in the
	 * directory hierarchy above the target, as this would
	 * orphan everything below the source directory. Also
	 * the user must have write permission in the source so
	 * as to be able to change "..".
	 */
	if (doingdirectory && newparent) {
        error = vnode_authorize(fvp, fdvp, KAUTH_VNODE_WRITE_DATA, context);
		if (error)
			goto unlockout;
		error = ufs_checkpath(ino, fdp->i_number, tdp, context,
		    &ino);
		/*
		 * We encountered a lock that we have to wait for.  Unlock
		 * everything else and VGET before restarting.
		 */
		if (ino) {
			iunlock(VTOI(fdvp));
			iunlock(VTOI(fvp));
			iunlock(VTOI(tdvp));
			if (tvp)
				iunlock(VTOI(tvp));
			error = VFS_VGET(mp, ino, &vargs, &nvp, context);
			if (error == 0)
				vnode_put(nvp);
			OSAddAtomic(1, &rename_restarts);
			goto relock;
		}
		if (error)
			goto unlockout;
		if ((tcnp->cn_flags & SAVESTART) == 0)
			panic("ufs_rename: lost to startdir");
	}
	if (fip->i_effnlink == 0 || fdp->i_effnlink == 0 ||
	    tdp->i_effnlink == 0)
		panic("Bad effnlink fip %p, fdp %p, tdp %p", fip, fdp, tdp);

	want_seqc_end = true;

	/*
	 * 1) Bump link count while we're moving stuff
	 *    around.  If we crash somewhere before
	 *    completing our work, the link count
	 *    may be wrong, but correctable.
	 */
	fip->i_effnlink++;
	fip->i_nlink++;
	DIP_SET(fip, i_nlink, fip->i_nlink);
	UFS_INODE_SET_FLAG(fip, IN_CHANGE);
	if (DOINGSOFTDEP(fvp))
		softdep_setup_link(tdp, fip);
	error = UFS_UPDATE(fvp, !DOINGSOFTDEP(fvp) && !DOINGASYNC(fvp));
	if (error)
		goto bad;

	/*
	 * 2) If target doesn't exist, link the target
	 *    to the source and unlink the source.
	 *    Otherwise, rewrite the target directory
	 *    entry to reference the source inode and
	 *    expunge the original entry's existence.
	 */
	if (tip == NULL) {
		if (ITODEV(tdp) != ITODEV(fip))
			panic("ufs_rename: EXDEV");
		if (doingdirectory && newparent) {
			/*
			 * Account for ".." in new directory.
			 * When source and destination have the same
			 * parent we don't adjust the link count.  The
			 * actual link modification is completed when
			 * .. is rewritten below.
			 */
			if (tdp->i_nlink >= UFS_LINK_MAX) {
				error = EMLINK;
				goto bad;
			}
		}
		ufs_makedirentry(fip, tcnp, &newdir);
		error = ufs_direnter(tdvp, NULL, &newdir, tcnp, NULL, 1, context);
		if (error)
			goto bad;
		/* Setup tdvp for directory compaction if needed. */
		if (I_COUNT(tdp) != 0 && I_ENDOFF(tdp) != 0 &&
		    I_ENDOFF(tdp) < tdp->i_size)
			endoff = I_ENDOFF(tdp);
	} else {
		if (ITODEV(tip) != ITODEV(tdp) || ITODEV(tip) != ITODEV(fip))
			panic("ufs_rename: EXDEV");
		/*
		 * Short circuit rename(foo, foo).
		 */
		if (tip->i_number == fip->i_number)
			panic("ufs_rename: same file");
		/*
		 * If the parent directory is "sticky", then the caller
		 * must possess VADMIN for the parent directory, or the
		 * destination of the rename.  This implements append-only
		 * directories.
		 */
        error = vnode_authorize(tdvp, NULLVP, KAUTH_VNODE_TAKE_OWNERSHIP, context);
        error1 = vnode_authorize(tvp, NULLVP, KAUTH_VNODE_TAKE_OWNERSHIP, context);
		if ((tdp->i_mode & S_ISTXT) && error && error1) {
			error = EPERM;
			goto bad;
		}
		/*
		 * Target must be empty if a directory and have no links
		 * to it. Also, ensure source and target are compatible
		 * (both directories, or both not directories).
		 */
		if ((tip->i_mode & IFMT) == IFDIR) {
			if ((tip->i_effnlink > 2) ||
			    !ufs_dirempty(tip, tdp->i_number, context)) {
				error = ENOTEMPTY;
				goto bad;
			}
			if (!doingdirectory) {
				error = ENOTDIR;
				goto bad;
			}
			cache_purge(tdvp);
		} else if (doingdirectory) {
			error = EISDIR;
			goto bad;
		}
		if (doingdirectory) {
			if (!newparent) {
				tdp->i_effnlink--;
				if (DOINGSOFTDEP(tdvp))
					softdep_change_linkcnt(tdp);
			}
			tip->i_effnlink--;
			if (DOINGSOFTDEP(tvp))
				softdep_change_linkcnt(tip);
		}
		error = ufs_dirrewrite(tdp, tip, fip->i_number,
		    IFTODT(fip->i_mode),
		    (doingdirectory && newparent) ? newparent : doingdirectory);
		if (error) {
			if (doingdirectory) {
				if (!newparent) {
					tdp->i_effnlink++;
					if (DOINGSOFTDEP(tdvp))
						softdep_change_linkcnt(tdp);
				}
				tip->i_effnlink++;
				if (DOINGSOFTDEP(tvp))
					softdep_change_linkcnt(tip);
			}
			goto bad;
		}
		if (doingdirectory && !DOINGSOFTDEP(tvp)) {
			/*
			 * The only stuff left in the directory is "."
			 * and "..". The "." reference is inconsequential
			 * since we are quashing it. We have removed the "."
			 * reference and the reference in the parent directory,
			 * but there may be other hard links. The soft
			 * dependency code will arrange to do these operations
			 * after the parent directory entry has been deleted on
			 * disk, so when running with that code we avoid doing
			 * them now.
			 */
			if (!newparent) {
				tdp->i_nlink--;
				DIP_SET(tdp, i_nlink, tdp->i_nlink);
				UFS_INODE_SET_FLAG(tdp, IN_CHANGE);
			}
			tip->i_nlink--;
			DIP_SET(tip, i_nlink, tip->i_nlink);
			UFS_INODE_SET_FLAG(tip, IN_CHANGE);
		}
	}

	/*
	 * 3) Unlink the source.  We have to resolve the path again to
	 * fixup the directory offset and count for ufs_dirremove.
	 */
	if (fdvp == tdvp) {
		error = ufs_lookup_ino(fdvp, NULL, fcnp, &ino, context);
		if (error)
			panic("ufs_rename: from entry went away!");
		if (ino != fip->i_number)
			panic("ufs_rename: ino mismatch %llu != %llu\n",
			    (uintmax_t)ino, (uintmax_t)fip->i_number);
	}
	/*
	 * If the source is a directory with a
	 * new parent, the link count of the old
	 * parent directory must be decremented
	 * and ".." set to point to the new parent.
	 */
	if (doingdirectory && newparent) {
		/*
		 * If tip exists we simply use its link, otherwise we must
		 * add a new one.
		 */
		if (tip == NULL) {
			tdp->i_effnlink++;
			tdp->i_nlink++;
			DIP_SET(tdp, i_nlink, tdp->i_nlink);
			UFS_INODE_SET_FLAG(tdp, IN_CHANGE);
			if (DOINGSOFTDEP(tdvp))
				softdep_setup_dotdot_link(tdp, fip);
			error = UFS_UPDATE(tdvp, !DOINGSOFTDEP(tdvp) &&
			    !DOINGASYNC(tdvp));
			/* Don't go to bad here as the new link exists. */
			if (error)
				goto unlockout;
		} else if (DOINGSUJ(tdvp))
			/* Journal must account for each new link. */
			softdep_setup_dotdot_link(tdp, fip);
		SET_I_OFFSET(fip, mastertemplate.dot_reclen);
		ufs_dirrewrite(fip, fdp, newparent, DT_DIR, 0);
		cache_purge(fdvp);
	}
	error = ufs_dirremove(fdvp, fip, fcnp->cn_flags, 0);
	/*
	 * The kern_renameat() looks up the fvp using the DELETE flag, which
	 * causes the removal of the name cache entry for fvp.
	 * As the relookup of the fvp is done in two steps:
	 * ufs_lookup_ino() and then VFS_VGET(), another thread might do a
	 * normal lookup of the from name just before the VFS_VGET() call,
	 * causing the cache entry to be re-instantiated.
	 *
	 * The same issue also applies to tvp if it exists as
	 * otherwise we may have a stale name cache entry for the new
	 * name that references the old i-node if it has other links
	 * or open file descriptors.
	 */
//	cache_vop_rename(fdvp, fvp, tdvp, tvp, fcnp, tcnp);
    /* update identity of inode in cache */
    vnode_update_identity(fvp, tdvp, tcnp->cn_nameptr, tcnp->cn_namelen,
                          tcnp->cn_hash, (VNODE_UPDATE_PARENT | VNODE_UPDATE_NAME));

unlockout:
    iunlock(VTOI(fdvp));
    iunlock(VTOI(fvp));
    iunlock(VTOI(tvp));
    
	vnode_put(fdvp);
	vnode_put(fvp);
	if (tvp)
		vnode_put(tvp);
	/*
	 * If compaction or fsync was requested do it now that other locks
	 * are no longer needed.
	 */
	if (error == 0 && endoff != 0) {
		do {
			error = UFS_TRUNCATE(tdvp, endoff, FREEBSD_IO_NORMAL |
			    (DOINGASYNC(tdvp) ? 0 : IO_SYNC), context);
		} while (error == ERECYCLE);
		if (error != 0 && !ffs_fsfail_cleanup(VFSTOUFS(mp), error))
			vn_printf(tdvp,
			    "ufs_rename: failed to truncate, error %d\n",
			    error);
#ifdef UFS_DIRHASH
		if (error != 0)
			ufsdirhash_free(tdp);
		else if (tdp->i_dirhash != NULL)
			ufsdirhash_dirtrunc(tdp, endoff);
#endif
		/*
		 * Even if the directory compaction failed, rename was
		 * succesful.  Do not propagate a UFS_TRUNCATE() error
		 * to the caller.
		 */
		error = 0;
	}
	if (error == 0 && tdp->i_flag & IN_NEEDSYNC) {
		do {
			error = VNOP_FSYNC(tdvp, MNT_WAIT, context);
		} while (error == ERECYCLE);
	}
	vnode_put(tdvp);
	return (error);

bad:
	fip->i_effnlink--;
	fip->i_nlink--;
	DIP_SET(fip, i_nlink, fip->i_nlink);
	UFS_INODE_SET_FLAG(fip, IN_CHANGE);
	if (DOINGSOFTDEP(fvp))
		softdep_revert_link(tdp, fip);
	goto unlockout;

releout:

	vnode_rele(fdvp);
	vnode_rele(fvp);
	vnode_rele(tdvp);
	if (tvp)
		vnode_rele(tvp);

	return (error);
}

#ifdef UFS_ACL
static int
ufs_do_posix1e_acl_inheritance_dir(struct vnode *dvp, struct vnode *tvp,
    mode_t dmode, struct ucred *cred, vfs_context_t context)
{
	int error;
	struct inode *ip = VTOI(tvp);
	struct acl *dacl, *acl;

	acl = acl_alloc(M_WAITOK);
	dacl = acl_alloc(M_WAITOK);

	/*
	 * Retrieve default ACL from parent, if any.
	 */
	error = VOP_GETACL(dvp, ACL_TYPE_DEFAULT, acl, cred, td);
	switch (error) {
	case 0:
		/*
		 * Retrieved a default ACL, so merge mode and ACL if
		 * necessary.  If the ACL is empty, fall through to
		 * the "not defined or available" case.
		 */
		if (acl->acl_cnt != 0) {
			dmode = acl_posix1e_newfilemode(dmode, acl);
			UFS_INODE_SET_MODE(ip, dmode);
			DIP_SET(ip, i_mode, dmode);
			*dacl = *acl;
			ufs_sync_acl_from_inode(ip, acl);
			break;
		}
		/* FALLTHROUGH */

	case EOPNOTSUPP:
		/*
		 * Just use the mode as-is.
		 */
		UFS_INODE_SET_MODE(ip, dmode);
		DIP_SET(ip, i_mode, dmode);
		error = 0;
		goto out;

	default:
		goto out;
	}

	/*
	 * XXX: If we abort now, will Soft Updates notify the extattr
	 * code that the EAs for the file need to be released?
	 */
	error = VOP_SETACL(tvp, ACL_TYPE_ACCESS, acl, cred, td);
	if (error == 0)
		error = VOP_SETACL(tvp, ACL_TYPE_DEFAULT, dacl, cred, td);
	switch (error) {
	case 0:
		break;

	case EOPNOTSUPP:
		/*
		 * XXX: This should not happen, as EOPNOTSUPP above
		 * was supposed to free acl.
		 */
		log_debug("ufs_mkdir: VOP_GETACL() but no VOP_SETACL()\n");
		/*
		panic("ufs_mkdir: VOP_GETACL() but no VOP_SETACL()");
		 */
		break;

	default:
		goto out;
	}

out:
	acl_free(acl);
	acl_free(dacl);

	return (error);
}

static int
ufs_do_posix1e_acl_inheritance_file(struct vnode *dvp, struct vnode *tvp,
    mode_t mode, struct ucred *cred, vfs_context_t context)
{
	int error;
	struct inode *ip = VTOI(tvp);
	struct acl *acl;

	acl = acl_alloc(M_WAITOK);

	/*
	 * Retrieve default ACL for parent, if any.
	 */
	error = VOP_GETACL(dvp, ACL_TYPE_DEFAULT, acl, cred, td);
	switch (error) {
	case 0:
		/*
		 * Retrieved a default ACL, so merge mode and ACL if
		 * necessary.
		 */
		if (acl->acl_cnt != 0) {
			/*
			 * Two possible ways for default ACL to not
			 * be present.  First, the EA can be
			 * undefined, or second, the default ACL can
			 * be blank.  If it's blank, fall through to
			 * the it's not defined case.
			 */
			mode = acl_posix1e_newfilemode(mode, acl);
			UFS_INODE_SET_MODE(ip, mode);
			DIP_SET(ip, i_mode, mode);
			ufs_sync_acl_from_inode(ip, acl);
			break;
		}
		/* FALLTHROUGH */

	case EOPNOTSUPP:
		/*
		 * Just use the mode as-is.
		 */
		UFS_INODE_SET_MODE(ip, mode);
		DIP_SET(ip, i_mode, mode);
		error = 0;
		goto out;

	default:
		goto out;
	}

	/*
	 * XXX: If we abort now, will Soft Updates notify the extattr
	 * code that the EAs for the file need to be released?
	 */
	error = VOP_SETACL(tvp, ACL_TYPE_ACCESS, acl, cred, td);
	switch (error) {
	case 0:
		break;

	case EOPNOTSUPP:
		/*
		 * XXX: This should not happen, as EOPNOTSUPP above was
		 * supposed to free acl.
		 */
		log_debug("ufs_do_posix1e_acl_inheritance_file: VOP_GETACL() "
		    "but no VOP_SETACL()\n");
		/* panic("ufs_do_posix1e_acl_inheritance_file: VOP_GETACL() "
		    "but no VOP_SETACL()"); */
		break;

	default:
		goto out;
	}

out:
	acl_free(acl);

	return (error);
}

static int
ufs_do_nfs4_acl_inheritance(struct vnode *dvp, struct vnode *tvp,
    mode_t child_mode, struct ucred *cred, vfs_context_t context)
{
	int error;
	struct acl *parent_aclp, *child_aclp;

	parent_aclp = acl_alloc(M_WAITOK);
	child_aclp = acl_alloc(M_WAITOK | M_ZERO);

	error = ufs_getacl_nfs4_internal(dvp, parent_aclp, td);
	if (error)
		goto out;
	acl_nfs4_compute_inherited_acl(parent_aclp, child_aclp,
	    child_mode, VTOI(tvp)->i_uid, vnode_vtype(tvp) == VDIR);
	error = ufs_setacl_nfs4_internal(tvp, child_aclp, td);
	if (error)
		goto out;
out:
	acl_free(parent_aclp);
	acl_free(child_aclp);

	return (error);
}
#endif

/*
 * Mkdir system call
 */
int
ufs_mkdir(struct vnop_mkdir_args *ap)
    /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
        struct vfs_context *a_context;
	} */
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode_attr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
    struct vfs_context *context = ap->a_context;
    struct ucred *cred = vfs_context_ucred(context);
	struct inode *ip, *dp;
	struct vnode *tvp;
	struct buf *bp;
	struct dirtemplate dirtemplate, *dtp;
	struct direct newdir;
	int error, dmode;
	long blkoff;

#ifdef INVARIANTS
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("ufs_mkdir: no name");
#endif
	dp = VTOI(dvp);
	if (dp->i_nlink >= UFS_LINK_MAX) {
		error = EMLINK;
		goto out;
	}
	dmode = vap->va_mode & 0777;
	dmode |= IFDIR;

	/*
	 * Must simulate part of ufs_makeinode here to acquire the inode,
	 * but not have it entered in the parent directory. The entry is
	 * made later after writing "." and ".." entries.
	 */
	if (dp->i_effnlink < 2) {
		print_bad_link_count("ufs_mkdir", dvp);
		error = EINVAL;
		goto out;
	}

	if (DOINGSOFTDEP(dvp)) {
		error = softdep_prelink(dvp, NULL, true);
		if (error != 0) {
			MPASS(error == ERECYCLE);
			return (error);
		}
	}

	error = UFS_VALLOC(dvp, dmode, context, &tvp);
	if (error)
		goto out;
	ip = VTOI(tvp);
	ip->i_gid = dp->i_gid;
	DIP_SET(ip, i_gid, dp->i_gid);
#ifdef SUIDDIR
	{
#ifdef QUOTA
		struct ucred ucred, *ucp;
		gid_t ucred_group;
		ucp = cnp->cn_cred;
#endif
		/*
		 * If we are hacking owners here, (only do this where told to)
		 * and we are not giving it TO root, (would subvert quotas)
		 * then go ahead and give it to the other user.
		 * The new directory also inherits the SUID bit.
		 * If user's UID and dir UID are the same,
		 * 'give it away' so that the SUID is still forced on.
		 */
		if ((vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_SUIDDIR) &&
		    (dp->i_mode & ISUID) && dp->i_uid) {
			dmode |= ISUID;
			ip->i_uid = dp->i_uid;
			DIP_SET(ip, i_uid, dp->i_uid);
#ifdef QUOTA
			if (dp->i_uid != cnp->cn_cred->cr_posix.cr_uid) {
				/*
				 * Make sure the correct user gets charged
				 * for the space.
				 * Make a dummy credential for the victim.
				 * XXX This seems to never be accessed out of
				 * our context so a stack variable is ok.
				 */
				refcount_init(&ucred.cr_ref, 1);
				ucred.cr_uid = ip->i_uid;
				ucred.cr_ngroups = 1;
				ucred.cr_groups = &ucred_group;
				ucred.cr_groups[0] = dp->i_gid;
				ucp = &ucred;
			}
#endif
		} else {
			ip->i_uid = cnp->cn_cred->cr_posix.cr_uid;
			DIP_SET(ip, i_uid, ip->i_uid);
		}
#ifdef QUOTA
		if ((error = getinoquota(ip)) ||
	    	    (error = chkiq(ip, 1, ucp, 0))) {
			if (DOINGSOFTDEP(tvp))
				softdep_revert_link(dp, ip);
			UFS_VFREE(tvp, ip->i_number, dmode);
			vn_revoke(tvp, 1, context);
			vnode_put(tvp);
			return (error);
		}
#endif
	}
#else	/* !SUIDDIR */
	ip->i_uid = cred->cr_posix.cr_uid;
	DIP_SET(ip, i_uid, ip->i_uid);
#ifdef QUOTA
	if ((error = getinoquota(ip)) ||
	    (error = chkiq(ip, 1, cnp->cn_cred, 0))) {
		if (DOINGSOFTDEP(tvp))
			softdep_revert_link(dp, ip);
		UFS_VFREE(tvp, ip->i_number, dmode);
		vn_revoke(tvp, 1, context);
		vnode_put(tvp);
		return (error);
	}
#endif
#endif	/* !SUIDDIR */
	UFS_INODE_SET_FLAG(ip, IN_ACCESS | IN_CHANGE | IN_UPDATE);
	UFS_INODE_SET_MODE(ip, dmode);
	DIP_SET(ip, i_mode, dmode);
	// tvp->v_vtype = VDIR;	/* Rest init'd in ufs_getnewvnode(). */
	ip->i_effnlink = 2;
	ip->i_nlink = 2;
	DIP_SET(ip, i_nlink, 2);

	if (cnp->cn_flags & ISWHITEOUT) {
		ip->i_flags |= UF_OPAQUE;
		DIP_SET(ip, i_flags, ip->i_flags);
	}

	/*
	 * Bump link count in parent directory to reflect work done below.
	 * Should be done before reference is created so cleanup is
	 * possible if we crash.
	 */
	dp->i_effnlink++;
	dp->i_nlink++;
	DIP_SET(dp, i_nlink, dp->i_nlink);
	UFS_INODE_SET_FLAG(dp, IN_CHANGE);
	if (DOINGSOFTDEP(dvp))
		softdep_setup_mkdir(dp, ip);
	error = UFS_UPDATE(dvp, !DOINGSOFTDEP(dvp) && !DOINGASYNC(dvp));
	if (error)
		goto bad;
#ifdef MAC
	if (vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_MULTILABEL) {
		error = mac_vnode_create_extattr(cnp->cn_cred, vnode_mount(dvp),
		    dvp, tvp, cnp);
		if (error)
			goto bad;
	}
#endif
#ifdef UFS_ACL
	if (vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_ACLS) {
		error = ufs_do_posix1e_acl_inheritance_dir(dvp, tvp, dmode,
		    cnp->cn_cred, cnp->cn_thread);
		if (error)
			goto bad;
	} else if (vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_NFS4ACLS) {
		error = ufs_do_nfs4_acl_inheritance(dvp, tvp, dmode,
		    cnp->cn_cred, cnp->cn_thread);
		if (error)
			goto bad;
	}
#endif /* !UFS_ACL */

	/*
	 * Initialize directory with "." and ".." from static template.
	 */
	if (vfs_maxsymlen(vnode_mount(dvp)) > 0)
		dtp = &mastertemplate;
	else
		dtp = (struct dirtemplate *)&omastertemplate;
	dirtemplate = *dtp;
	dirtemplate.dot_ino = ip->i_number;
	dirtemplate.dotdot_ino = dp->i_number;
	ubc_setsize(tvp, DIRBLKSIZ);
	if ((error = UFS_BALLOC(tvp, (off_t)0, DIRBLKSIZ, context, BA_CLRBUF, &bp)) != 0)
		goto bad;
	ip->i_size = DIRBLKSIZ;
	DIP_SET(ip, i_size, DIRBLKSIZ);
	UFS_INODE_SET_FLAG(ip, IN_SIZEMOD | IN_CHANGE | IN_UPDATE);
	bcopy((caddr_t)&dirtemplate, (caddr_t)buf_dataptr(bp), sizeof dirtemplate);
	if (DOINGSOFTDEP(tvp)) {
		/*
		 * Ensure that the entire newly allocated block is a
		 * valid directory so that future growth within the
		 * block does not have to ensure that the block is
		 * written before the inode.
		 */
		blkoff = DIRBLKSIZ;
		while (blkoff < buf_count(bp)) {
			((struct direct *)
			   (buf_dataptr(bp) + blkoff))->d_reclen = DIRBLKSIZ;
			blkoff += DIRBLKSIZ;
		}
	}
	if ((error = UFS_UPDATE(tvp, !DOINGSOFTDEP(tvp) &&
	    !DOINGASYNC(tvp))) != 0) {
		(void)bwrite(bp);
		goto bad;
	}
	/*
	 * Directory set up, now install its entry in the parent directory.
	 *
	 * If we are not doing soft dependencies, then we must write out the
	 * buffer containing the new directory body before entering the new 
	 * name in the parent. If we are doing soft dependencies, then the
	 * buffer containing the new directory body will be passed to and
	 * released in the soft dependency code after the code has attached
	 * an appropriate ordering dependency to the buffer which ensures that
	 * the buffer is written before the new name is written in the parent.
	 */
	if (DOINGASYNC(dvp))
		buf_bdwrite(bp);
	else if (!DOINGSOFTDEP(dvp) && ((error = bwrite(bp))))
		goto bad;
	ufs_makedirentry(ip, cnp, &newdir);
	error = ufs_direnter(dvp, tvp, &newdir, cnp, bp, 0, context);

bad:
	if (error == 0) {
		*ap->a_vpp = tvp;
	} else {
		dp->i_effnlink--;
		dp->i_nlink--;
		DIP_SET(dp, i_nlink, dp->i_nlink);
		UFS_INODE_SET_FLAG(dp, IN_CHANGE);
		/*
		 * No need to do an explicit VOP_TRUNCATE here, vnode_rele will
		 * do this for us because we set the link count to 0.
		 */
		ip->i_effnlink = 0;
		ip->i_nlink = 0;
		DIP_SET(ip, i_nlink, 0);
		UFS_INODE_SET_FLAG(ip, IN_CHANGE);
		if (DOINGSOFTDEP(tvp))
			softdep_revert_mkdir(dp, ip);
		vn_revoke(tvp, 1, context);
		vnode_put(tvp);
	}
out:
	return (error);
}

errno_t VNOP_MKDIR(vnode_t dvp, vnode_t *vpp, struct componentname *cnp,
                   struct vnode_attr *vap, vfs_context_t ctx)
{
    struct vnop_mkdir_args a;
    a.a_desc = &vnop_mkdir_desc;
    a.a_dvp = dvp;
    a.a_vpp = vpp;
    a.a_cnp = cnp;
    a.a_vap = vap;
    a.a_context = ctx;
    return ufs_mkdir(&a);
}

/*
 * Rmdir system call.
 */
int
ufs_rmdir(struct vnop_rmdir_args *ap)
    /* {
		struct vnode *a_dvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
        struct vfs_context *a_context;
	} */
{
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
    struct vfs_context *context = ap->a_context;
	struct inode *ip, *dp;
	int error;

	ip = VTOI(vp);
	dp = VTOI(dvp);

	/*
	 * Do not remove a directory that is in the process of being renamed.
	 * Verify the directory is empty (and valid). Rmdir ".." will not be
	 * valid since ".." will contain a reference to the current directory
	 * and thus be non-empty. Do not allow the removal of mounted on
	 * directories (this can happen when an NFS exported filesystem
	 * tries to remove a locally mounted on directory).
	 */
	error = 0;
	if (dp->i_effnlink <= 2) {
		if (dp->i_effnlink == 2)
			print_bad_link_count("ufs_rmdir", dvp);
		error = EINVAL;
		goto out;
	}
	if (!ufs_dirempty(ip, dp->i_number, context)) {
		error = ENOTEMPTY;
		goto out;
	}
	if ((dp->i_flags & APPEND)
	    || (ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND))) {
		error = EPERM;
		goto out;
	}
	if (vnode_ismountedon(vp) != 0) {
		error = EINVAL;
		goto out;
	}
	if (DOINGSOFTDEP(dvp)) {
		error = softdep_prelink(dvp, vp, false);
		if (error != 0) {
			MPASS(error == ERECYCLE);
			return (error);
		}
	}

	/*
	 * Delete reference to directory before purging
	 * inode.  If we crash in between, the directory
	 * will be reattached to lost+found,
	 */
	dp->i_effnlink--;
	ip->i_effnlink--;
	if (DOINGSOFTDEP(vp))
		softdep_setup_rmdir(dp, ip);
	error = ufs_dirremove(dvp, ip, cnp->cn_flags, 1);
	if (error) {
		dp->i_effnlink++;
		ip->i_effnlink++;
		if (DOINGSOFTDEP(vp))
			softdep_revert_rmdir(dp, ip);
		goto out;
	}
	/*
	 * The only stuff left in the directory is "." and "..". The "."
	 * reference is inconsequential since we are quashing it. The soft
	 * dependency code will arrange to do these operations after
	 * the parent directory entry has been deleted on disk, so
	 * when running with that code we avoid doing them now.
	 */
	if (!DOINGSOFTDEP(vp)) {
		dp->i_nlink--;
		DIP_SET(dp, i_nlink, dp->i_nlink);
		UFS_INODE_SET_FLAG(dp, IN_CHANGE);
		error = UFS_UPDATE(dvp, 0);
		ip->i_nlink--;
		DIP_SET(ip, i_nlink, ip->i_nlink);
		UFS_INODE_SET_FLAG(ip, IN_CHANGE);
	}
//	cache_vop_rmdir(dvp, vp);
    vnode_update_identity(vp, dvp, cnp->cn_nameptr, cnp->cn_namelen,
                          cnp->cn_hash, VNODE_UPDATE_PURGE | VNODE_UPDATE_PARENT);
#ifdef UFS_DIRHASH
	/* Kill any active hash; i_effnlink == 0, so it will not come back. */
	if (ip->i_dirhash != NULL)
		ufsdirhash_free(ip);
#endif
out:
	return (error);
}

/*
 * symlink -- make a symbolic link
 */
int
ufs_symlink(struct vnop_symlink_args *ap)
    /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		const char *a_target;
        struct vfs_context *a_context;
	} */
{
	struct vnode *vp, **vpp = ap->a_vpp;
	struct inode *ip;
	int len, error;

	error = ufs_makeinode(IFLNK | ap->a_vap->va_mode, ap->a_dvp,
                          vpp, ap->a_cnp, "ufs_symlink", ap->a_context);
	if (error)
		return (error);
	vp = *vpp;
	len = (int)strlen(ap->a_target);
	if (len < vfs_maxsymlen(vnode_mount(vp))) {
		ip = VTOI(vp);
		bcopy(ap->a_target, SHORTLINK(ip), len);
		ip->i_size = len;
		DIP_SET(ip, i_size, len);
		UFS_INODE_SET_FLAG(ip, IN_SIZEMOD | IN_CHANGE | IN_UPDATE);
		error = UFS_UPDATE(vp, 0);
	} else
		error = vn_rdwr(UIO_WRITE, vp, __DECONST(void *, ap->a_target),
		    len, (off_t)0, UIO_SYSSPACE, IO_NODELOCKED | FREEBSD_IO_NOMACCHECK,
                        NOCRED, NULL, NULL);
	if (error)
		vnode_put(vp);
	return (error);
}

/*
 * Return target name of a symbolic link
 */
int
ufs_readlink(struct vnop_readlink_args *ap)
    /* {
		struct vnode *a_vp;
		struct uio *a_uio;
        vfs_context_t a_context;
	} */
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	doff_t isize;

	isize = (unsigned) ip->i_size;
	if ((isize < vfs_maxsymlen(vnode_mount(vp))) ||
	    DIP(ip, i_blocks) == 0) { /* XXX - for old fastlink support */
		return (uiomove(SHORTLINK(ip), isize, ap->a_uio));
	}
	return (VNOP_READ(vp, ap->a_uio, 0, ap->a_context));
}

/*
 * Calculate the logical to physical mapping if not done already,
 * then call the device strategy routine.
 *
 * In order to be able to swap to a file, the ufs_bmaparray() operation may not
 * deadlock on memory.  See ufs_bmap() for details.
 */
int
ufs_strategy(struct vnop_strategy_args *ap)
    /* {
		struct vnode *a_vp;
		struct buf *a_bp;
	} */
{
    struct buf *bp = ap->a_bp;
    struct vnode *vp = buf_vnode(bp);
    struct inode *ip = VTOI(vp);
    int error;

    trace_enter();
    error = buf_strategy(ip->i_devvp, ap);
    trace_return (error);
}

/*
 * Read wrapper for fifo
 */
int
ufsfifo_read(struct vnop_read_args *ap)
{
    VTOI(ap->a_vp)->i_flag |= IN_ACCESS;
    return fifo_read(ap);
}

/*
 * Write wrapper for fifo
 */
int
ufsfifo_write(struct vnop_write_args *ap)
{
    VTOI(ap->a_vp)->i_flag |= IN_CHANGE | IN_UPDATE;
    return fifo_write(ap);
}


/*
 * Close wrapper for fifos.
 *
 * Update the times on the inode then do device close.
 */
int
ufsfifo_close(struct vnop_close_args *ap)
    /* {
		struct vnode *a_vp;
		int  a_fflag;
		struct ucred *a_cred;
		struct vfs_context *a_td;
	} */
{
	struct vnode *vp = ap->a_vp;
    struct inode *ip = VTOI(vp);

	ixlock(ip);
	if (vnode_isinuse(vp, 1))
		ufs_itimes_locked(ip);
	iunlock(ip);
    extern int fifo_close(struct vnop_close_args *);
	return (fifo_close(ap));
}

/*
 * Return POSIX pathconf information applicable to ufs filesystems.
 */
int
ufs_pathconf(struct vnop_pathconf_args *ap)
    /* {
		struct vnode *a_vp;
		int a_name;
		int *a_retval;
	} */
{
	int error;

	error = 0;
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = UFS_LINK_MAX;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = UFS_MAXNAMLEN;
		break;
	case _PC_PIPE_BUF:
		if (vnode_vtype(ap->a_vp) == VDIR || vnode_vtype(ap->a_vp) == VFIFO)
			*ap->a_retval = PIPE_BUF;
		else
			error = EINVAL;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		break;
#ifdef UFS_ACL
	case _PC_ACL_EXTENDED:
		if (vfs_flags(vnode_mount(ap->a_vp)) & FREEBSD_MNT_ACLS)
			*ap->a_retval = 1;
		else
			*ap->a_retval = 0;
		break;
	case _PC_ACL_NFS4:
		if (vfs_flags(vnode_mount(ap->a_vp)) & FREEBSD_MNT_NFS4ACLS)
			*ap->a_retval = 1;
		else
			*ap->a_retval = 0;
		break;
#endif
	case _PC_ACL_PATH_MAX:
#ifdef UFS_ACL
		if (vfs_flags(vnode_mount(ap->a_vp)) & (FREEBSD_MNT_ACLS | FREEBSD_MNT_NFS4ACLS))
			*ap->a_retval = ACL_MAX_ENTRIES;
		else
			*ap->a_retval = 3;
#else
		*ap->a_retval = 3;
#endif
		break;
#ifdef MAC
	case _PC_MAC_PRESENT:
		if (vfs_flags(vnode_mount(ap->a_vp)) & FREEBSD_MNT_MULTILABEL)
			*ap->a_retval = 1;
		else
			*ap->a_retval = 0;
		break;
#endif
	case _PC_MIN_HOLE_SIZE:
		*ap->a_retval = (int)vfs_statfs(vnode_mount(ap->a_vp))->f_iosize;
		break;
	case _PC_PRIO_IO:
		*ap->a_retval = 0;
		break;
	case _PC_SYNC_IO:
		*ap->a_retval = 0;
		break;
	case _PC_ALLOC_SIZE_MIN:
		*ap->a_retval = (int)vfs_statfs(vnode_mount(ap->a_vp))->f_bsize;
		break;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		break;
	case _PC_REC_INCR_XFER_SIZE:
		*ap->a_retval = (int)vfs_statfs(vnode_mount(ap->a_vp))->f_iosize;
		break;
	case _PC_REC_MAX_XFER_SIZE:
		*ap->a_retval = -1; /* means ``unlimited'' */
		break;
	case _PC_REC_MIN_XFER_SIZE:
		*ap->a_retval = (int)vfs_statfs(vnode_mount(ap->a_vp))->f_iosize;
		break;
	case _PC_REC_XFER_ALIGN:
		*ap->a_retval = PAGE_SIZE;
		break;
	case _PC_SYMLINK_MAX:
		*ap->a_retval = MAXPATHLEN;
		break;

	default:
        error = EINVAL;
		break;
	}
	return (error);
}


static inline u_quad_t init_filerev(){
    struct timespec ts;
    getnanotime(&ts);
    return (((u_quad_t)ts.tv_sec << 32LL) | (ts.tv_nsec >> 32LL));
}

/*
 * Initialize the vnode associated with a new inode, handle aliased
 * vnodes.
 */
int
ufs_getnewvnode(struct mount *mp, struct vnode_init_args *vap, struct vnode **vpp)
{
    struct vnode_fsparam vfsargs = {0};
    struct componentname *cnp;
    struct inode *ip;
    struct vnode *vp;
    char *name;
    int error;

    name = NULL;
    *vpp = vp = NULL;
    ip = vap->vi_ip;
    cnp = vap->vi_cnp;
    
    trace_enter();
    
    vfsargs.vnfs_mp = ip->i_ump->um_mountp;
    vfsargs.vnfs_vtype = IFTOVT(ip->i_mode);
    vfsargs.vnfs_str = "ufsOnOSX";
    vfsargs.vnfs_dvp = vap->vi_parent;
    vfsargs.vnfs_fsnode = ip;
    vfsargs.vnfs_vops = ufsX_vnodeops_p;
    vfsargs.vnfs_markroot = (UFS_ROOTINO == ip->i_number);
    // file inodes used for FS meta-data - don't believe this is ever the case in ext2/3
    vfsargs.vnfs_marksystem = (UFS_SNAPDIR_INO == ip->i_number);
    vfsargs.vnfs_filesize = ip->i_size;
    vfsargs.vnfs_cnp = cnp;
    
    if (!vfsargs.vnfs_dvp || (cnp && !(cnp->cn_flags & MAKEENTRY))){
        vfsargs.vnfs_flags = VNFS_NOCACHE;
    }
    
    if (VNON == vfsargs.vnfs_vtype)
        return (ENOENT);
    
    switch(vfsargs.vnfs_vtype) {
        case VCHR:
        case VBLK:
            vfsargs.vnfs_vops = ufsX_specops_p;
            vfsargs.vnfs_rdev = (dev_t)DIP(ip, i_rdev);
            break;
        case VFIFO:
            vfsargs.vnfs_vops = ufsX_fifoops_p;
            break;
        default:
            break;
    }
    
    /*
     * Initialize modrev times
     */
    if (I_IS_UFS1(ip)) {
        ip->i_din1->di_modrev = init_filerev();
    } else {
        ip->i_din2->di_modrev = init_filerev();
    }

    
    /* vnode_initialize and vnode_create_empty are private apis */
    if ((error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsargs, &vp)) != 0)
        return (error);
    
    // update component name if we came from valloc and this inode wasn't reclaimed.
    if (cnp != NULL && ISSET(vap->vi_flags, VINIT_VALLOC)){
        int id_flags = VNODE_UPDATE_NAME;
        if (vap->vi_parent != NULLVP)
            id_flags |= VNODE_UPDATE_PARENT;
        vnode_update_identity(vp, vap->vi_parent, cnp->cn_nameptr, cnp->cn_namelen, cnp->cn_hash, id_flags);
        
        if (vfsargs.vnfs_markroot){
            strncpy(ip->i_name, "/", sizeof("/") - 1);
        } else if (cnp->cn_nameptr != NULL){
            strncpy(ip->i_name, cnp->cn_nameptr, cnp->cn_namelen);
        } else {
            name = (char*)vnode_getname(vp);
            strncpy(ip->i_name, name, strlen(name));
            vnode_putname(name);
        }
    }
    
    vnode_ref(ip->i_devvp);
    vnode_addfsref(vp);
    vnode_settag(vp, VT_UFS);
    // set vnode pointer
    ip->i_vnode = *vpp = vp;
    
    trace_return (0);
}

/*
 * Allocate a new inode.
 * Vnode dvp must be locked.
 */
static int
ufs_makeinode(int mode, struct vnode *dvp, struct vnode **vpp, struct componentname *cnp, const char *callfunc, struct vfs_context *context)
{
	struct inode *ip, *pdir;
	struct direct newdir;
	struct vnode *tvp;
    struct ucred *cred;
	int error;

	pdir = VTOI(dvp);
#ifdef INVARIANTS
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("%s: no name", callfunc);
#endif
	*vpp = NULL;
	if ((mode & IFMT) == 0)
		mode |= IFREG;

	if (pdir->i_effnlink < 2) {
		print_bad_link_count(callfunc, dvp);
		return (EINVAL);
	}
	if (DOINGSOFTDEP(dvp)) {
		error = softdep_prelink(dvp, NULL, true);
		if (error != 0) {
			MPASS(error == ERECYCLE);
			return (error);
		}
	}
	error = UFS_VALLOC(dvp, mode, context, &tvp);
	if (error)
		return (error);
	ip = VTOI(tvp);
    cred = vfs_context_ucred(context);
	ip->i_gid = pdir->i_gid;
	DIP_SET(ip, i_gid, pdir->i_gid);
#ifdef SUIDDIR
	{
#ifdef QUOTA
		struct ucred ucred, *ucp;
		gid_t ucred_group;
		ucp = cnp->cn_cred;
#endif
		/*
		 * If we are not the owner of the directory,
		 * and we are hacking owners here, (only do this where told to)
		 * and we are not giving it TO root, (would subvert quotas)
		 * then go ahead and give it to the other user.
		 * Note that this drops off the execute bits for security.
		 */
		if ((vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_SUIDDIR) &&
		    (pdir->i_mode & ISUID) &&
		    (pdir->i_uid != cnp->cn_cred->cr_posix.cr_uid) && pdir->i_uid) {
			ip->i_uid = pdir->i_uid;
			DIP_SET(ip, i_uid, ip->i_uid);
			mode &= ~07111;
#ifdef QUOTA
			/*
			 * Make sure the correct user gets charged
			 * for the space.
			 * Quickly knock up a dummy credential for the victim.
			 * XXX This seems to never be accessed out of our
			 * context so a stack variable is ok.
			 */
			refcount_init(&ucred.cr_ref, 1);
			ucred.cr_uid = ip->i_uid;
			ucred.cr_ngroups = 1;
			ucred.cr_groups = &ucred_group;
			ucred.cr_groups[0] = pdir->i_gid;
			ucp = &ucred;
#endif
		} else {
			ip->i_uid = cnp->cn_cred->cr_posix.cr_uid;
			DIP_SET(ip, i_uid, ip->i_uid);
		}

#ifdef QUOTA
		if ((error = getinoquota(ip)) ||
	    	    (error = chkiq(ip, 1, ucp, 0))) {
			if (DOINGSOFTDEP(tvp))
				softdep_revert_link(pdir, ip);
			UFS_VFREE(tvp, ip->i_number, mode);
			vn_revoke(tvp, 1, context);
			vnode_put(tvp);
			return (error);
		}
#endif
	}
#else	/* !SUIDDIR */
	ip->i_uid = cred->cr_posix.cr_uid;
	DIP_SET(ip, i_uid, ip->i_uid);
#ifdef QUOTA
	if ((error = getinoquota(ip)) ||
	    (error = chkiq(ip, 1, cnp->cn_cred, 0))) {
		if (DOINGSOFTDEP(tvp))
			softdep_revert_link(pdir, ip);
		UFS_VFREE(tvp, ip->i_number, mode);
		vn_revoke(tvp, 1, context);
		vnode_put(tvp);
		return (error);
	}
#endif
#endif	/* !SUIDDIR */
	UFS_INODE_SET_FLAG(ip, IN_ACCESS | IN_CHANGE | IN_UPDATE);
	UFS_INODE_SET_MODE(ip, mode);
	DIP_SET(ip, i_mode, mode);
//	vnode_vtype(tvp) = IFTOVT(mode);	/* Rest init'd in ufs_getnewvnode(). */
	ip->i_effnlink = 1;
	ip->i_nlink = 1;
	DIP_SET(ip, i_nlink, 1);
	if (DOINGSOFTDEP(tvp))
		softdep_setup_create(VTOI(dvp), ip);
        if ((ip->i_mode & ISGID) &&
            !groupmember(ip->i_gid, cred) &&
            !kauth_cred_issuser(cred)) {
		UFS_INODE_SET_MODE(ip, ip->i_mode & ~ISGID);
		DIP_SET(ip, i_mode, ip->i_mode);
	}

	if (cnp->cn_flags & ISWHITEOUT) {
		ip->i_flags |= UF_OPAQUE;
		DIP_SET(ip, i_flags, ip->i_flags);
	}

	/*
	 * Make sure inode goes to disk before directory entry.
	 */
	error = UFS_UPDATE(tvp, !DOINGSOFTDEP(tvp) && !DOINGASYNC(tvp));
	if (error)
		goto bad;
#ifdef MAC
	if (vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_MULTILABEL) {
		error = mac_vnode_create_extattr(cnp->cn_cred, vnode_mount(dvp),
		    dvp, tvp, cnp);
		if (error)
			goto bad;
	}
#endif
#ifdef UFS_ACL
	if (vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_ACLS) {
		error = ufs_do_posix1e_acl_inheritance_file(dvp, tvp, mode,
		    cnp->cn_cred, cnp->cn_thread);
		if (error)
			goto bad;
	} else if (vfs_flags(vnode_mount(dvp)) & FREEBSD_MNT_NFS4ACLS) {
		error = ufs_do_nfs4_acl_inheritance(dvp, tvp, mode,
		    cnp->cn_cred, cnp->cn_thread);
		if (error)
			goto bad;
	}
#endif /* !UFS_ACL */
	ufs_makedirentry(ip, cnp, &newdir);
	error = ufs_direnter(dvp, tvp, &newdir, cnp, NULL, 0, context);
	if (error)
		goto bad;
	*vpp = tvp;
	return (0);

bad:
	/*
	 * Write error occurred trying to update the inode
	 * or the directory so must deallocate the inode.
	 */
	ip->i_effnlink = 0;
	ip->i_nlink = 0;
	DIP_SET(ip, i_nlink, 0);
	UFS_INODE_SET_FLAG(ip, IN_CHANGE);
	if (DOINGSOFTDEP(tvp))
		softdep_revert_create(VTOI(dvp), ip);
	vn_revoke(tvp, 1, context);
	vnode_put(tvp);
	return (error);
}

int
ufs_ioctl(struct vnop_ioctl_args *ap)
{
	struct vnode *vp;
	int error;
    extern int ffs_susp_ioctl(struct vnop_ioctl_args *ap);

	vp = ap->a_vp;
    switch (ap->a_command) {
        case FSCTL_FIOSEEKDATA:
            error = vn_lock(vp, UFS_LOCK_SHARED);
            if (error == 0) {
                error = ufs_bmap_seekdata(vp, (off_t *)ap->a_data);
                VNOP_UNLOCK(vp);
            } else
                error = EBADF;
            return (error);
        case FSCTL_FIOSEEKHOLE:
            return ufs_bmap_seekhole(vp, ap->a_desc, ap->a_command, (off_t *)ap->a_data, ap->a_context);
        case FSCTL_UFSSUSPEND:
        case FSCTL_UFSRESUME:
            return ffs_susp_ioctl(ap);
        default:
            return (ENOTTY);
    }
    
}

/*
 * Read wrapper for special devices.
 */
int
ufsspec_read(   struct vnop_read_args *ap)
    /* {
        struct vnode *a_vp;
        struct uio *a_uio;
        int  a_ioflag;
        vfs_context_t a_context;
    } */
{

    /*
     * Set access flag.
     */
    VTOI(ap->a_vp)->i_flag |= IN_ACCESS;
    return spec_read(ap);
}

/*
 * Write wrapper for special devices.
 */
int
ufsspec_write(
    struct vnop_write_args /* {
        struct vnode *a_vp;
        struct uio *a_uio;
        int  a_ioflag;
        kauth_cred_t a_cred;
    } */ *ap)
{

    /*
     * Set update and change flags.
     */
    VTOI(ap->a_vp)->i_flag |= IN_CHANGE | IN_UPDATE;
    return spec_write(ap);
}

int
ufsspec_close(   struct vnop_close_args *ap)
    /* {
        struct vnode *a_vp;
        int  a_fflag;
        vfs_context_t a_context;
    } */
{
    struct vnode *vp = ap->a_vp;
    struct inode *ip = VTOI(vp);
    
    if (vnode_isinuse(vp, 1)) {
        ixlock(ip);
        ufs_itimes_locked(ip);
        iunlock(ip);
    }
    return spec_close(ap);
}

/*
 * Vnode op for pagein.
 * Similar to ext2_read()
 */
int
ffs_pagein(struct vnop_pagein_args *ap)
    /* {
        vnode_t a_vp,
        upl_t     a_pl,
        vm_offset_t   a_pl_offset,
        off_t         a_f_offset,
        size_t        a_size,
        struct ucred *a_cred,
        int           a_flags
    } */
{
    register vnode_t vp;
    register struct inode *ip;
    int error = 0;

    vp = ap->a_vp;
    ip = VTOI(vp);
    
    trace_enter();
    
#if DIAGNOSTIC
    if (VLNK == vnode_vtype(vp)) {
        if ((int)ip->i_size < vfs_maxsymlen(vnode_mount(vp)))
            panic("ext2_pagein: short symlink");
    } else if (VREG != vnode_vtype(vp) && VDIR != vnode_vtype(vp))
        panic("ext2_pagein: type %d", vnode_vtype(vp));
#endif
    
    if (ap->a_pl != NULL) {
        error = cluster_pagein(vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset,
                               (int)ap->a_size, ip->i_size, ap->a_flags);
        goto out;
    }

//    if ((vfs_flags(vnode_mount(ip->i_vnode)) & MNT_NOATIME) == 0)
//        ip->i_flags |= IN_ACCESS;
out:
    trace_return (error);
}

/*
 * Vnode op for pageout.
 * Similar to ext2_vnop_write()
 * make sure the buf is not in hash queue when you return
 */
int
ffs_pageout(struct vnop_pageout_args *ap)
    /* {
       vnode_t a_vp,
       upl_t        a_pl,
       vm_offset_t   a_pl_offset,
       off_t         a_f_offset,
       size_t        a_size,
       struct ucred *a_cred,
       int           a_flags
    } */
{
    vnode_t vp;
    upl_t pl;
    int flags;
    off_t f_offset;
    upl_offset_t pl_offset;
    upl_size_t po_size; // pageout size in bytes
    struct ucred *cred;
    
    struct inode *ip;
    struct ufsmount *ump;
    struct fs *fs;
    int error, locktype;
    
    size_t f_size;
    upl_size_t xfer_size = 0, xsize;
    int local_flags = 0;
    off_t local_offset;
    int resid, blkoffset;
    daddr64_t lbn;
    int alloc_error = 0;
    upl_size_t lupl_offset;
    bool should_cleanup; // specifies whether we should cleanup the upl.

    trace_enter();
    
    vp = ap->a_vp;
    pl = ap->a_pl;
    flags = ap->a_flags;
    f_offset = ap->a_f_offset;
    pl_offset = lupl_offset = ap->a_pl_offset;
    po_size = resid = (upl_size_t)ap->a_size;
    cred = vfs_context_ucred(ap->a_context);

    error = 0;
    ip = VTOI(vp);
    ump = ip->i_ump;
    fs = ump->um_fs;
    should_cleanup = (flags & UPL_NOCOMMIT) == 0;
    locktype = inode_lock_owned(ip);
    
    if (fs->fs_ronly) {
        error = EROFS;
        goto out_err;
    }

    if(locktype == 0){
        ixlock(ip);
    } else if (locktype == UFS_LOCK_SHARED){
        iupgradelock(ip);
    }
    
    f_size = ip->i_size;
    
    // make sure file offset is within the bounds of our file
    // if the requested file offset exceeds our file size, abort.
    if (f_offset < 0 || f_offset >= f_size) {
        error = EINVAL;
        goto out_err;
    }
    
    /*
     * once we enable multi-page pageouts we will
     * need to make sure we abort any pages in the upl
     * that we don't issue an I/O for
     */
    if (f_offset + po_size > f_size)
        xfer_size = (int) (f_size - f_offset);
    else
        xfer_size = po_size;

    /* if not a multiple of page size
     * then round up to be a multiple
     * the physical disk block size
     */
    if (xfer_size & (PAGE_SIZE - 1))
        xfer_size =  roundup(xfer_size, fs->fs_bsize);

    /*
     * once the block allocation is moved to ext2_blockmap
     * we can remove all the size and offset checks above
     * cluster_pageout does all of this now
     * we need to continue to do it here so as not to
     * allocate blocks that aren't going to be used because
     * of a bogus parameter being passed in
     */

    resid = xfer_size;
    local_offset = f_offset;
    
//    for (error = 0; resid > 0;) {
//        resid -= xsize;
//        local_offset += (off_t)xsize;
//    }

    if (error) {
        // if the the offset is same as before, no allocation was done. just abort range and return.
        if (local_offset == f_offset){
            lupl_offset = pl_offset;
            goto out_err;
        }
        // let's write the allocated blocks
        alloc_error = error;
        xfer_size -= resid;
    }

    error = cluster_pageout(vp, pl, pl_offset, f_offset, round_page_32(xfer_size), f_size, flags);
    
//    error = cluster_pageout_ext(vp, pl, pl_offset, f_offset, round_page_32(xfer_size),
//                                f_size, flags, buf_bio_callback, &ext2_vnop_pageout);

    lupl_offset = po_size - resid;
    
    if (error) {
        goto out_err;
    } else if(alloc_error) {
        // if there was no problem writing out pages, return the alloc error instead
        // 'error' is definitely zero here
        error = alloc_error;
        goto out_err;
    }
    
    trace_return (0);
out_err:
    
    if (should_cleanup)
        ubc_upl_abort_range(pl, lupl_offset, round_page_32(resid), UPL_ABORT_FREE_ON_EMPTY);
    trace_return (error);
}

#pragma mark FIFO OPERATIONS

/*
 * Read wrapper for fifos.
 */
static int ufs_fifo_read(struct vnop_read_args *ap)
    /* {
        vnode_t a_vp;
        struct uio *a_uio;
        int  a_ioflag;
        ucred_ta_cred;
    } */
{
    int error;
    size_t resid;
    vnode_t vp;
    struct inode *ip;
    struct uio *uio;
   
   log_debug("Enter");

    uio = ap->a_uio;
    resid = uio_resid(uio);
    error = fifo_read(ap);
    vp = ap->a_vp;
    ip = VTOI(vp);
    if ((vfs_flags(vnode_mount(vp)) & MNT_NOATIME) == 0 && ip != NULL &&
        (uio_resid(uio) != resid || (error == 0 && resid != 0))) {
        ixlock(ip);
        ip->i_flags |= IN_ACCESS;
        iunlock(ip);
    }
    trace_return(error);
}

/*
 * Write wrapper for fifos.
 */
static int ufs_fifo_write(struct vnop_write_args *ap)
    /* {
        vnode_t a_vp;
        struct uio *a_uio;
        int  a_ioflag;
        ucred_ta_cred;
    } */
{
    int error;
    size_t resid;
    struct inode *ip;
    struct uio *uio;
   
    log_debug("Enter");

    uio = ap->a_uio;
    resid = uio_resid(uio);
    error = fifo_write(ap);
    ip = VTOI(ap->a_vp);
    if (ip != NULL && (uio_resid(uio) != resid || (error == 0 && resid != 0))) {
        ixlock(ip);
        ip->i_flags |= IN_CHANGE | IN_UPDATE;
        iunlock(ip);
    }
    trace_return(error);
}

static int ufs_fifo_bmap(struct vnop_blockmap_args *ap)
/*
    struct vnop_blockmap_args {
        struct vnode    *a_vp;
        off_t        a_foffset;
        size_t        a_size;
        daddr64_t    *a_bpn;
        size_t        *a_run;
        void        *a_poff;
        int        a_flags;
*/
{
    log_debug("fifo vnop_blockmap\n");
    trace_return (ENOTSUP);
}

/*
 * Close wrapper for fifos.
 *
 * Update the times on the inode then do device close.
 */
static int ufs_fifo_close(struct vnop_close_args *ap)
{
    struct vnode *vp = ap->a_vp;
    struct inode *ip = VTOI(vp);

    ixlock(ip);
    if (vnode_isinuse(vp, 1))
        ufs_itimes_locked(ip);
    iunlock(ip);
    trace_return (fifo_close(ap));
}


// VNOP vectors
/* Global vfs data structures for ufs. */
vnop_t**ufsX_vnodeops_p;
struct vnodeopv_entry_desc ufsX_vnodeops[] = {
    { &vnop_default_desc,           (vnop_t*)vn_default_error       },
    { &vnop_fsync_desc,             (vnop_t*)ffs_fsync              },
    { &vnop_read_desc,              (vnop_t*)ffs_read               },
    { &vnop_write_desc,             (vnop_t*)ffs_write              },
    { &vnop_blockmap_desc,          (vnop_t*)ufs_bmap               },
    { &vnop_close_desc,             (vnop_t*)ufs_vnop_close         },
    { &vnop_create_desc,            (vnop_t*)ufs_create             },
    { &vnop_getattr_desc,           (vnop_t*)ufs_getattr            },
    { &vnop_inactive_desc,          (vnop_t*)ufs_inactive           },
    { &vnop_ioctl_desc,             (vnop_t*)ufs_ioctl              },
    { &vnop_link_desc,              (vnop_t*)ufs_link               },
    { &vnop_lookup_desc,            (vnop_t*)ufs_lookup             },
    { &vnop_mmap_desc,              (vnop_t*)ufs_mmap               },
    { &vnop_mkdir_desc,             (vnop_t*)ufs_mkdir              },
    { &vnop_mknod_desc,             (vnop_t*)ufs_mknod              },
    { &vnop_open_desc,              (vnop_t*)ufs_open               },
    { &vnop_pathconf_desc,          (vnop_t*)ufs_pathconf           },
    { &vnop_readdir_desc,           (vnop_t*)ufs_readdir            },
    { &vnop_readlink_desc,          (vnop_t*)ufs_readlink           },
    { &vnop_reclaim_desc,           (vnop_t*)ufs_reclaim            },
    { &vnop_remove_desc,            (vnop_t*)ufs_remove             },
    { &vnop_rename_desc,            (vnop_t*)ufs_rename             },
    { &vnop_rmdir_desc,             (vnop_t*)ufs_rmdir              },
    { &vnop_setattr_desc,           (vnop_t*)ufs_setattr            },
    { &vnop_strategy_desc,          (vnop_t*)ufs_strategy           },
    { &vnop_symlink_desc,           (vnop_t*)ufs_symlink            },
    { &vnop_whiteout_desc,          (vnop_t*)ufs_whiteout           },
    
    { &vnop_copyfile_desc,          (vnop_t*)err_copyfile           },
    { &vnop_pagein_desc,            (vnop_t*)ffs_pagein             },
    { &vnop_pageout_desc,           (vnop_t*)ffs_pageout            },
    { &vnop_blktooff_desc,          (vnop_t*)ffs_blktooff           },
    { &vnop_offtoblk_desc,          (vnop_t*)ffs_offtoblk           },
    { &vnop_bwrite_desc,            (vnop_t*)vn_bwrite              },
#ifdef UFS_ACL
    { &vnop_getacl_desc,            (vnop_t*)ufs_getacl             },
    { &vnop_setacl_desc,            (vnop_t*)ufs_setacl             },
    { &vnop_aclcheck_desc,          (vnop_t*)ufs_aclcheck           },
#endif
    { NULL, NULL },
};
struct vnodeopv_desc ufsX_vnodeop_opv_desc = { &ufsX_vnodeops_p, ufsX_vnodeops };

vnop_t**ufsX_specops_p;
struct vnodeopv_entry_desc ufsX_specops[] = {
    { &vnop_default_desc,           (vnop_t*)vn_default_error   },
    { &vnop_lookup_desc,            (vnop_t*)spec_lookup        },
    { &vnop_create_desc,            (vnop_t*)spec_create        },
    { &vnop_mknod_desc,             (vnop_t*)spec_mknod         },
    { &vnop_open_desc,              (vnop_t*)spec_open          },
    { &vnop_close_desc,             (vnop_t*)ufsspec_close      },
    { &vnop_getattr_desc,           (vnop_t*)ufs_getattr        },
    { &vnop_setattr_desc,           (vnop_t*)ufs_setattr        },
    { &vnop_read_desc,              (vnop_t*)ufsspec_read       },
    { &vnop_write_desc,             (vnop_t*)ufsspec_write      },
    { &vnop_ioctl_desc,             (vnop_t*)spec_ioctl         },
    { &vnop_select_desc,            (vnop_t*)spec_select        },
    { &vnop_revoke_desc,            (vnop_t*)spec_revoke        },
    { &vnop_mmap_desc,              (vnop_t*)spec_mmap          },
    { &vnop_fsync_desc,             (vnop_t*)ffs_fsync          },
    { &vnop_remove_desc,            (vnop_t*)spec_remove        },
    { &vnop_link_desc,              (vnop_t*)spec_link          },
    { &vnop_rename_desc,            (vnop_t*)spec_rename        },
    { &vnop_mkdir_desc,             (vnop_t*)spec_mkdir         },
    { &vnop_rmdir_desc,             (vnop_t*)spec_rmdir         },
    { &vnop_symlink_desc,           (vnop_t*)spec_symlink       },
    { &vnop_readdir_desc,           (vnop_t*)spec_readdir       },
    { &vnop_readlink_desc,          (vnop_t*)spec_readlink      },
    { &vnop_inactive_desc,          (vnop_t*)ufs_inactive       },
    { &vnop_reclaim_desc,           (vnop_t*)ufs_reclaim        },
    { &vnop_strategy_desc,          (vnop_t*)spec_strategy      },
    { &vnop_pathconf_desc,          (vnop_t*)spec_pathconf      },
    { &vnop_advlock_desc,           (vnop_t*)err_advlock        },
    { &vnop_bwrite_desc,            (vnop_t*)vn_bwrite          },
    
    { &vnop_pagein_desc,            (vnop_t*)ffs_pagein         },
    { &vnop_pageout_desc,           (vnop_t*)ffs_pageout        },
    { &vnop_copyfile_desc,          (vnop_t*)err_copyfile       },
    { &vnop_blktooff_desc,          (vnop_t*)ffs_blktooff       },
    { &vnop_offtoblk_desc,          (vnop_t*)ffs_offtoblk       },
    { &vnop_blockmap_desc,          (vnop_t*)err_blockmap       },
    { NULL, NULL },
};
struct vnodeopv_desc ufsX_specop_opv_desc = { &ufsX_specops_p, ufsX_specops };

vnop_t**ufsX_fifoops_p;
static struct vnodeopv_entry_desc ufsX_fifoops[] = {
    { &vnop_default_desc,           (vnop_t *) vn_default_error     },
    { &vnop_close_desc,             (vnop_t *) ufs_fifo_close       },
    { &vnop_fsync_desc,             (vnop_t *) ffs_fsync            },
    { &vnop_getattr_desc,           (vnop_t *) ufs_getattr          },
    { &vnop_inactive_desc,          (vnop_t *) ufs_inactive         },
    { &vnop_read_desc,              (vnop_t *) ufs_fifo_read        },
    { &vnop_reclaim_desc,           (vnop_t *) ufs_reclaim          },
    { &vnop_setattr_desc,           (vnop_t *) ufs_setattr          },
    { &vnop_write_desc,             (vnop_t *) ufs_fifo_write       },

    { &vnop_advlock_desc,           (vnop_t *) err_advlock          },
    { &vnop_blockmap_desc,          (vnop_t *) ufs_fifo_bmap        },       /*fifo_blockmap*/ 
    { &vnop_lookup_desc,            (vnop_t *) fifo_lookup          },
    { &vnop_create_desc,            (vnop_t *) fifo_create          },
    { &vnop_link_desc,              (vnop_t *) fifo_link            },
    { &vnop_mkdir_desc,             (vnop_t *) fifo_mkdir           },
    { &vnop_mknod_desc,             (vnop_t *) fifo_mknod           },
    { &vnop_open_desc,              (vnop_t *) fifo_open            },
    { &vnop_pathconf_desc,          (vnop_t *) fifo_pathconf        },
    { &vnop_readdir_desc,           (vnop_t *) fifo_readdir         },
    { &vnop_readlink_desc,          (vnop_t *) fifo_readlink        },
    { &vnop_remove_desc,            (vnop_t *) fifo_remove          },
    { &vnop_rename_desc,            (vnop_t *) fifo_rename          },
    { &vnop_rmdir_desc,             (vnop_t *) fifo_rmdir           },
    { &vnop_mmap_desc,              (vnop_t *) fifo_mmap            },        /* mmap */
    { &vnop_ioctl_desc,             (vnop_t *) fifo_ioctl           },
    { &vnop_strategy_desc,          (vnop_t *) err_strategy         },
    { &vnop_bwrite_desc,            (vnop_t *) vn_bwrite            },
    
    { &vnop_pagein_desc,            (vnop_t *) ffs_pagein           },
    { &vnop_pageout_desc,           (vnop_t *) ffs_pageout          },
    { &vnop_blktooff_desc,          (vnop_t *) ffs_blktooff         },
    { &vnop_offtoblk_desc,          (vnop_t *) ffs_offtoblk         },
    { &vnop_copyfile_desc,          (vnop_t *) err_copyfile         },        /* copyfile */
    { NULL, NULL }
};
struct vnodeopv_desc ufsX_fifoop_opv_desc = { &ufsX_fifoops_p, ufsX_fifoops };
