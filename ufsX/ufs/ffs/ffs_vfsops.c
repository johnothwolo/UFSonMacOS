/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1989, 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)ffs_vfsops.c	8.31 (Berkeley) 5/20/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/disk.h>
#include <sys/mount.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/ioccom.h>
#include <sys/sysctl.h>
#include <kern/assert.h>
#include <sys/vnode_if.h>

#include <freebsd/compat/compat.h>

#include <ufs/ufsX_vfsops.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

#include <sys/ubc.h>

struct ffs_cbargs {
    struct vfs_context *context;
    int waitfor;
    int error;
};

static int  ffs_reload_callback(struct vnode *vp, void *arg);
static int	ffs_mountfs(struct vnode *, struct mount *, struct vfs_context *);
static void	ffs_oldfscompat_read(struct fs *, struct ufsmount *, ufs2_daddr_t);
static void	ffs_ifree(struct ufsmount *ump, struct inode *ip);
static int	ffs_sync_lazy(struct mount *mp, struct vfs_context *);
static int	ffs_use_bread(void *devfd, off_t loc, void **bufp, int size);
static int	ffs_use_bwrite(void *devfd, off_t loc, void *buf, int size);

int  ffs_vgetf(struct mount *mp, ino_t ino, struct vfs_vget_args *ap,
               struct vnode **vpp, vfs_context_t context);

struct vfsops ffs_vfsops = {
    .vfs_mount          = ffs_mount,
    .vfs_start          = ffs_start,
    .vfs_unmount        = ffs_unmount,
    .vfs_root           = ufs_root,
    .vfs_quotactl       = ufs_quotactl,
    .vfs_getattr        = ffs_getattrfs,
    .vfs_sync           = ffs_sync,
    .vfs_vget           = ffs_vget,
    .vfs_fhtovp         = ffs_fhtovp,
    .vfs_vptofh         = ffs_vptofh,
    .vfs_init           = ffs_init,
    .vfs_sysctl         = ffs_sysctl,
    .vfs_setattr        = ffs_setattrfs,
    .vfs_ioctl          = NULL,/*ffs_ioctl,*/
    .vfs_vget_snapdir   = NULL /*ffs_vget_snapdir,*/
};

static bo_strategy_t ffs_geom_strategy;
static bo_write_t ffs_bufwrite;

__unused
static struct buf_ops ffs_ops = {
	.bop_name =	"FFS",
	.bop_write =	ffs_bufwrite,
	.bop_strategy =	ffs_geom_strategy,
	.bop_sync =	bufsync,
	.bop_bdflush =	ffs_bdflush,
};

/*
 * Note that userquota and groupquota options are not currently used
 * by UFS/FFS code and generally mount(8) does not pass those options
 * from userland, but they can be passed by loader(8) via
 * vfs.root.mountfrom.options.
 */
__unused
static const char *ffs_opts[] = {
    "acls", "async", "noatime", "noexec", "export", "force", "from", "groupquota",
    "multilabel", "nfsv4acls", "fsckpid", "snapshot", "nosuid", "suiddir",
    "nosymfollow", "sync", "union", "userquota", "untrusted", NULL
};

static int ffs_enxio_enable = 1;
SYSCTL_DECL(_vfs_ffs);
SYSCTL_INT(_vfs_ffs, OID_AUTO, enxio_enable, CTLFLAG_RW, &ffs_enxio_enable, 0,
    "enable mapping of other disk I/O errors to ENXIO");

int
bwrite(buf_t bp)
{
    // We can't use VNOP_BWRITE here, because ffs_bufwrite
    // is meant to be used synchronously.
//    int ffs_bufwrite(struct buf *bp);
//    return ffs_bufwrite(bp);
    return buf_bwrite(bp);
}

int ffs_start(struct mount *mp, int flags, struct vfs_context *ctx)
{
    trace_return(0);
}



/*
 * Return buffer with the contents of block "offset" from the beginning of
 * directory "ip".  If "res" is non-zero, fill it in with a pointer to the
 * remaining space in the directory.
 */
static int
ffs_blkatoff(struct vnode *vp, off_t offset, char **res, struct buf **bpp)
{
	struct inode *ip;
	struct fs *fs;
	struct buf *bp;
	ufs_lbn_t lbn;
	int bsize, error;

    trace_enter();
    
	ip = VTOI(vp);
	fs = ITOFS(ip);
	lbn = lblkno(fs, offset);
	bsize = blksize(fs, ip, lbn);
    
    log_debug("offset=%llu lbn=%llu bsize=%d", offset, lbn, bsize);

	*bpp = NULL;
	error = buf_meta_bread(vp, lbn, bsize, NOCRED, &bp);
	if (error) {
		trace_return (error);
	}
	if (res)
		*res = (char *)buf_dataptr(bp) + blkoff(fs, offset);
	*bpp = bp;
	trace_return (0);
}

/*
 * Load up the contents of an inode and copy the appropriate pieces
 * to the incore copy.
 */
static int
ffs_load_inode(struct buf *bp, struct inode *ip, struct fs *fs, ino_t ino)
{
	struct ufs1_dinode *dip1;
	struct ufs2_dinode *dip2;
	int error;

	if (I_IS_UFS1(ip)) {
		dip1 = ip->i_din1;
		*dip1 = *((struct ufs1_dinode *)buf_dataptr(bp) + ino_to_fsbo(fs, ino));
		ip->i_mode = dip1->di_mode;
		ip->i_nlink = dip1->di_nlink;
		ip->i_effnlink = dip1->di_nlink;
		ip->i_size = dip1->di_size;
		ip->i_flags = dip1->di_flags;
		ip->i_gen = dip1->di_gen;
		ip->i_uid = dip1->di_uid;
		ip->i_gid = dip1->di_gid;
		return (0);
	}
	dip2 = ((struct ufs2_dinode *)buf_dataptr(bp) + ino_to_fsbo(fs, ino));
	if ((error = ffs_verify_dinode_ckhash(fs, dip2)) != 0 &&
	    !ffs_fsfail_cleanup(ITOUMP(ip), error)) {
		log_debug("%s: inode %lld: check-hash failed\n", fs->fs_fsmnt, (intmax_t)ino);
		return (error);
	}
	*ip->i_din2 = *dip2;
	dip2 = ip->i_din2;
	ip->i_mode = dip2->di_mode;
	ip->i_nlink = dip2->di_nlink;
	ip->i_effnlink = dip2->di_nlink;
	ip->i_size = dip2->di_size;
	ip->i_flags = dip2->di_flags;
	ip->i_gen = dip2->di_gen;
	ip->i_uid = dip2->di_uid;
	ip->i_gid = dip2->di_gid;
	return (0);
}

/*
 * Verify that a filesystem block number is a valid data block.
 * This routine is only called on untrusted filesystems.
 */
static int
ffs_check_blkno(struct mount *mp, ino_t inum, ufs2_daddr_t daddr, int blksize, int havemtx)
{
	struct fs *fs;
	struct ufsmount *ump;
	ufs2_daddr_t end_daddr;
	int cg;

	ASSERT((vfs_flags(mp) & FREEBSD_MNT_UNTRUSTED) != 0, ("ffs_check_blkno called on a trusted file system"));
	ump = VFSTOUFS(mp);
	fs = ump->um_fs;
	cg = (int) dtog(fs, daddr);
	end_daddr = daddr + numfrags(fs, blksize);
	/*
	 * Verify that the block number is a valid data block. Also check
	 * that it does not point to an inode block or a superblock. Accept
	 * blocks that are unalloacted (0) or part of snapshot metadata
	 * (BLK_NOCOPY or BLK_SNAP).
	 *
	 * Thus, the block must be in a valid range for the filesystem and
	 * either in the space before a backup superblock (except the first
	 * cylinder group where that space is used by the bootstrap code) or
	 * after the inode blocks and before the end of the cylinder group.
	 */
	if ((uint64_t)daddr <= BLK_SNAP ||
	    ((uint64_t)end_daddr <= fs->fs_size &&
	    ((cg > 0 && end_daddr <= cgsblock(fs, cg)) ||
	    (daddr >= cgdmin(fs, cg) &&
	    end_daddr <= cgbase(fs, cg) + fs->fs_fpg))))
		return (0);
    if (havemtx){
        LCK_MTX_ASSERT(UFS_MTX(ump), LCK_MTX_ASSERT_OWNED);
    } else
		UFS_LOCK(ump);
	if (ppsratecheck(&ump->um_last_integritymsg, &ump->um_secs_integritymsg, 1)) {
		UFS_UNLOCK(ump);
        log_debug("\n%s: inode %u, out-of-range indirect block "
		    "number %lld\n", vfs_statfs(mp)->f_mntonname, inum, daddr);
		if (havemtx)
			UFS_LOCK(ump);
	} else if (!havemtx)
		UFS_UNLOCK(ump);
	return (ESTALE);
}


/*
 * On first ENXIO error, start a task that forcibly unmounts the filesystem.
 *
 * Return true if a cleanup is in progress.
 */
int
ffs_fsfail_cleanup(struct ufsmount *ump, int error)
{
	int retval;

	UFS_LOCK(ump);
	retval = ffs_fsfail_cleanup_locked(ump, error);
	UFS_UNLOCK(ump);
	return (retval);
}

int
ffs_fsfail_cleanup_locked(struct ufsmount *ump, int error)
{
    struct vfsstatfs *statfs = vfs_statfs(ump->um_mountp);
	LCK_MTX_ASSERT(UFS_MTX(ump), LCK_ASSERT_OWNED);
	if (error == ENXIO && (ump->um_flags & UM_FSFAIL_CLEANUP) == 0) {
		ump->um_flags |= UM_FSFAIL_CLEANUP;
		/*
		 * Queue a forced unmount.
		 */
        log_debug("UFS: forcibly unmounting %s from %s\n", statfs->f_mntfromname, statfs->f_mntonname);

        if (ump->um_mountp != NULL){
            vfs_unmountbyfsid(&statfs->f_fsid, MNT_FORCE | MNT_DEFWRITE, vfs_context_current());
        }
	}
	return ((ump->um_flags & UM_FSFAIL_CLEANUP) != 0);
}

/*
 * Wrapper used during ENXIO cleanup to allocate empty buffers when
 * the kernel is unable to read the real one. They are needed so that
 * the soft updates code can use them to unwind its dependencies.
 */
int
ffs_bread(struct ufsmount *ump, struct vnode *vp, daddr64_t lblkno,
    daddr64_t dblkno, int size, daddr64_t *rablkno, int *rabsize, int cnt,
    struct ucred *cred, int flags, void (*ckhashfunc)(struct buf *),
    struct buf **bpp)
{
	int error;
	error = breadn_flags(vp, lblkno, lblkno, size, rablkno, rabsize, cnt, cred, flags, ckhashfunc, bpp);
	if (error != 0 && ffs_fsfail_cleanup(ump, error)) {
		*bpp = getblk(vp, lblkno, size, 0, 0, BLK_READ);
		ASSERT(error == 0, ("getblkx failed"));
		buf_clear(*bpp);
	}
	return (error);
}

int
ffs_meta_bread(struct ufsmount *ump, struct vnode *vp, daddr64_t lblkno, int size,
               struct ucred *cred, int flags, void (*ckhashfunc)(struct buf *), struct buf **bpp)
{
    int error;
    
    error = meta_bread_flags(vp, lblkno, size, cred, flags, ckhashfunc, bpp);
    if (error != 0 && ffs_fsfail_cleanup(ump, error)) {
        *bpp = buf_getblk(vp, lblkno, size, 0, 0, BLK_READ | BLK_META);
        ASSERT(error == 0, ("getblkx failed"));
        buf_clear(*bpp);
    }
    return (error);
}

int
ffs_mount(struct mount *mp, struct vnode* devvp, user_addr_t data, struct vfs_context *context)
{
	struct ufsmount *ump = NULL;
	struct fs *fs;
	pid_t fsckpid = 0;
	int error, flags, devbsize;
	uint64_t saved_mnt_flag;
    struct ufs_args *args, _args;
	char *fspec, *path;
    struct vfsstatfs *statfs;
    bool doingsoftdeps, doingsuj;
    
    devbsize = DEV_BSIZE;
    statfs = vfs_statfs(mp);
    fspec = statfs->f_mntfromname;
    path  = statfs->f_mntonname;
    doingsoftdeps = doingsuj = false;
    
    log_debug("Mounting fs at %s", fspec);
    
    if ((error = copyin(CAST_USER_ADDR_T(data), &_args, sizeof(struct ufs_args))) == 0){
        args = &_args;
    } else {
        args = NULL;
        log_debug("waarning!!! no args provided. Falling back to defaults");
    }
    
    if (VNOP_IOCTL(devvp, DKIOCSETBLOCKSIZE, (char*)&devbsize, 0, vfs_context_current()))
        panic("cannot get physical sector-size");
    
    
#ifndef WANTRDWR
    vfs_setflags(mp, MNT_RDONLY);
#endif
    // TODO: implement supds and suj.
    if (args){
        args->ufs_mntflags &= ~(FREEBSD_MNT_SOFTDEP | FREEBSD_MNT_SUJ);
        fsckpid = args->ufs_fsckpid;
    }
    
#ifndef QUOTA
    vfs_clearflags(mp, MNT_QUOTA);
#endif
    if (args->ufs_mntflags & FREEBSD_MNT_UNTRUSTED)
		vfs_setflags(mp, MNT_QUARANTINE);
#ifdef SNAPSHOTS
	if (vfs_flags(mp) & MNT_SNAPSHOT) {
		/*
		 * Once we have set the MNT_SNAPSHOT flag, do not
		 * persist "snapshot" in the options list.
		 */
        vfs_clearflags(mp, MNT_QUARANTINE);
	}
#else
    if (vfs_flags(mp) & MNT_SNAPSHOT) {
        vfs_clearflags(mp, MNT_SNAPSHOT);
    }
#endif
    
//    if (args.ufs_fsckpid) {
//		/*
//		 * Once we have set the restricted PID, do not
//		 * persist "fsckpid" in the options list.
//		 */
//		if (vfs_isupdate(mp)) {
//			if (VFSTOUFS(mp)->um_fs->fs_ronly == 0 && vfs_isrdonly(mp)) {
//				log_debug("Checker enable: Must be read-only");
//				return (EINVAL);
//			}
//		} else if (vfs_isrdwr(mp)) {
//			log_debug("Checker enable: Must be read-only");
//			return (EINVAL);
//		}
//		/* Set to -1 if we are done */
//		if (fsckpid == 0)
//			fsckpid = -1;
//	}

    if ((args->ufs_mntflags & (FREEBSD_MNT_ACLS | FREEBSD_MNT_NFS4ACLS)) ==
        (FREEBSD_MNT_ACLS | FREEBSD_MNT_NFS4ACLS)) {
			log_debug("\"acls\" and \"nfsv4acls\" options are mutually exclusive");
			return (EINVAL);
	}
    
	/*
	 * If updating, check whether changing from read-only to
	 * read/write; if there is no device name, that's all we do.
	 */
	if (vfs_isupdate(mp)) {
		ump = VFSTOUFS(mp);
		fs = ump->um_fs;
		if (fsckpid == -1 && ump->um_fsckpid > 0) {
			if ((error = ffs_flushfiles(mp, WRITECLOSE, context)) != 0 ||
			    (error = ffs_sbupdate(ump, MNT_WAIT, 0)) != 0)
				return (error);
			ump->um_fsckpid = 0;
		}
		if (fs->fs_ronly == 0 && vfs_isrdonly(mp)) {
			/*
			 * Check for and optionally get rid of files open
			 * for writing.
			 */
			flags = WRITECLOSE;
			if (vfs_isforce(mp))
				flags |= FORCECLOSE;
			if (args->ufs_mntflags & (FREEBSD_MNT_SOFTDEP | FREEBSD_MNT_SUJ)) {
				error = softdep_flushfiles(mp, flags, context);
			} else {
				error = ffs_flushfiles(mp, flags, context);
			}
			if (fs->fs_pendingblocks != 0 ||
			    fs->fs_pendinginodes != 0) {
				log_debug("WARNING: %s Update error: blocks %lld "
				    "files %d\n", fs->fs_fsmnt, 
				    (intmax_t)fs->fs_pendingblocks,
				    fs->fs_pendinginodes);
				fs->fs_pendingblocks = 0;
				fs->fs_pendinginodes = 0;
			}
			if ((fs->fs_flags & (FS_UNCLEAN | FS_NEEDSFSCK)) == 0)
				fs->fs_clean = 1;
			if ((error = ffs_sbupdate(ump, MNT_WAIT, 0)) != 0) {
				fs->fs_ronly = 0;
				fs->fs_clean = 0;
				return (error);
			}
			if (MOUNTEDSOFTDEP(mp))
				softdep_unmount(mp);
            
			fs->fs_ronly = 1;
			vfs_setflags(mp, MNT_RDONLY);
		}
		if (vfs_isreload(mp) &&
		    (error = ffs_reload(mp, context, 0)) != 0)
			return (error);
		if (fs->fs_ronly && !vfs_isrdonly(mp)) {
			/*
			 * If we are running a checker, do not allow upgrade.
			 */
//			if (ump->um_fsckpid > 0) {
//				log_debug("Active checker, cannot upgrade to write");
//				return (EINVAL);
//			}
            
            // note: MNT_FORCE spcifies that it's for unmount(8)
            // but the kernel leaves the flag on if specified for mount(8)
			fs->fs_flags &= ~FS_UNCLEAN;
			if (fs->fs_clean == 0) {
				fs->fs_flags |= FS_UNCLEAN;
				if (vfs_isforce(mp) ||
				    ((fs->fs_flags &
				     (FS_SUJ | FS_NEEDSFSCK)) == 0 &&
				     (fs->fs_flags & FS_DOSOFTDEP))) {
					log_debug("WARNING: %s was not properly dismounted\n", fs->fs_fsmnt);
				} else {
					log_debug("R/W mount of %s denied. %s.%s", fs->fs_fsmnt,
					   "Filesystem is not clean - run fsck",
					   (fs->fs_flags & FS_SUJ) == 0 ? "" :
					   " Forced mount will invalidate"
					   " journal contents");
					return (EPERM);
				}
			}
            
			fs->fs_ronly = 0;
			saved_mnt_flag = MNT_RDONLY;
			if (MOUNTEDSOFTDEP(mp) && !vfs_issynchronous(mp))
				saved_mnt_flag |= MNT_ASYNC;
            vfs_clearflags(mp, saved_mnt_flag);
			fs->fs_mtime = time_seconds();
			/* check to see if we need to start softdep */
//			if ((fs->fs_flags & FS_DOSOFTDEP) && (error = softdep_mount(devvp, mp, fs, context))){
//                // MARK: we revert these because the mount is already mounted
//                // MARK: so we leave the mount in the same state if the upgrade fails
//				fs->fs_ronly = 1;
//				vfs_setflags(mp, saved_mnt_flag);
//				vfs_write_resume(mp, 0);
//				return (error);
//			}
			fs->fs_clean = 0;
			if ((error = ffs_sbupdate(ump, MNT_WAIT, 0)) != 0) {
				fs->fs_ronly = 1;
				vfs_setflags(mp, saved_mnt_flag);
				vfs_write_resume(mp, 0);
				return (error);
			}
//			if (fs->fs_snapinum[0] != 0)
//				ffs_snapshot_mount(mp);
			vfs_write_resume(mp, 0);
		}
		/*
		 * Soft updates is incompatible with "async",
		 * so if we are doing softupdates stop the user
		 * from setting the async flag in an update.
		 * Softdep_mount() clears it in an initial mount
		 * or ro->rw remount.
		 */
		if (MOUNTEDSOFTDEP(mp)) {
			/* XXX: Reset too late ? */
			vfs_clearflags(mp, MNT_ASYNC);
		}
		/*
		 * Keep MNT_ACLS flag if it is stored in superblock.
		 */
		if ((fs->fs_flags & (FS_ACLS | FS_NFS4ACLS)) != 0) {
			/* XXX: Set too late ? */
            vfs_setextendedsecurity(mp); // force ACLs
		}

		/*
		 * If this is a request from fsck to clean up the filesystem,
		 * then allow the specified pid to proceed.
		 */
		if (fsckpid > 0) {
			if (ump->um_fsckpid != 0) {
				log_debug(
				    "Active checker already running on %s",
				    fs->fs_fsmnt);
				return (EINVAL);
			}
			ASSERT(MOUNTEDSOFTDEP(mp) == 0, ("soft updates enabled on read-only file system"));

			ump->um_fsckpid = fsckpid;
//			if (fs->fs_snapinum[0] != 0)
//				ffs_snapshot_mount(mp);
			fs->fs_mtime = time_seconds();
			fs->fs_fmod = 1;
			fs->fs_clean = 0;
			(void) ffs_sbupdate(ump, MNT_WAIT, 0);
		}

		/*
		 * If this is a snapshot request, take the snapshot.
		 */
        if ((vfs_flags(mp) & MNT_SNAPSHOT) != 0)
			return (ffs_snapshot(mp, fspec));

		/*
		 * Must not call namei() while owning busy ref.
		 */
		vfs_unbusy(mp);
	}

	/*
	 * Not an update, or updating the name: look up the name
	 * and verify that it refers to a sensible disk device.
	 */
	if (vfs_isupdate(mp)) {
		/*
		 * Unmount does not start if MNT_UPDATE is set.  Mount
		 * update busies mp before setting MNT_UPDATE.  We
		 * must be able to retain our busy ref succesfully,
		 * without sleep.
		 */
		error = vfs_busy(mp, LK_NOWAIT);
		MPASS(error == 0);
        
		/*
		 * Update only
		 *
		 * If it's not the same vnode, or at least the same device
		 * then it's not correct.
		 */
		if (vnode_specrdev(devvp) != vnode_specrdev(ump->um_devvp))
			return (EINVAL);
	} else {
		/*
		 * New mount
		 *
		 * We need the name for the mount point (also used for
		 * "last mounted on") copied in. If an error occurs,
		 * the mount point is discarded by the upper level code.
		 * Note that vfs_mount_alloc() populates f_mntonname for us.
		 */
		if ((error = ffs_mountfs(devvp, mp, context)) != 0) {
            log_debug("ffs_mountfs() failed with error %d", error);
			return (error);
		}
		if (fsckpid > 0) {
			ASSERT(MOUNTEDSOFTDEP(mp) == 0,
			    ("soft updates enabled on read-only file system"));
			ump = VFSTOUFS(mp);
			fs = ump->um_fs;
			if (error) {
				log_debug("WARNING: %s: Checker activation "
				    "failed\n", fs->fs_fsmnt);
			} else { 
				ump->um_fsckpid = fsckpid;
//				if (fs->fs_snapinum[0] != 0)
//					ffs_snapshot_mount(mp);
				fs->fs_mtime = time_seconds();
				fs->fs_clean = 0;
				(void) ffs_sbupdate(ump, MNT_WAIT, 0);
			}
		}
	}

	return (0);
}

/*
 * Reload all incore data for a filesystem (used after running fsck on
 * the root filesystem and finding things to fix). If the 'force' flag
 * is 0, the filesystem must be mounted read-only.
 *
 * Things to do to update the mount:
 *	1) invalidate all cached meta-data.
 *	2) re-read superblock from disk.
 *	3) re-read summary information from disk.
 *	4) invalidate all inactive vnodes.
 *	5) clear MNTK_SUSPEND2 and MNTK_SUSPENDED flags, allowing secondary
 *	   writers, if requested.
 *	6) invalidate all cached file data.
 *	7) re-read inode data for all active vnodes.
 */
int
ffs_reload(struct mount *mp, vfs_context_t context, int flags)
{
	struct vnode *devvp;
	void *space;
	struct buf *bp;
	struct fs *fs, *newfs;
	struct ufsmount *ump;
    struct bsdmount *bmp;
    struct ffs_cbargs reload_iterargs;
	ufs2_daddr_t sblockloc;
    int i, blks, error;
	u_long size;
	int32_t *lp;

	ump = VFSTOUFS(mp);
    bmp = ump->um_compat;

	if (!vfs_isrdonly(mp) && (flags & FFSR_FORCE) == 0) {
		return (EINVAL);
	}

	/*
	 * Step 1: invalidate all cached meta-data.
	 */
	devvp = VFSTOUFS(mp)->um_devvp;
	if (buf_invalidateblks(devvp, BUF_WRITE_DATA, 0, 0) != 0)
		panic("ffs_reload: dirty1");

	/*
	 * Step 2: re-read superblock from disk.
	 */
	fs = VFSTOUFS(mp)->um_fs;
	if ((error = buf_meta_bread(devvp, btodb(fs->fs_sblockloc, DEV_BSIZE), fs->fs_sbsize, NOCRED, &bp)) != 0)
		return (error);
	newfs = (struct fs *)buf_dataptr(bp);
	if ((newfs->fs_magic != FS_UFS1_MAGIC &&
	     newfs->fs_magic != FS_UFS2_MAGIC) ||
	    newfs->fs_bsize > MAXBSIZE ||
	    newfs->fs_bsize < sizeof(struct fs)) {
			buf_brelse(bp);
			return (EIO);		/* XXX needs translation */
	}
	/*
	 * Preserve the summary information, read-only status, and
	 * superblock location by copying these fields into our new
	 * superblock before using it to update the existing superblock.
	 */
	newfs->fs_si = fs->fs_si;
	newfs->fs_ronly = fs->fs_ronly;
	sblockloc = fs->fs_sblockloc;
	bcopy(newfs, fs, (u_int)fs->fs_sbsize);
	buf_brelse(bp);
	vfs_setmaxsymlen(mp, fs->fs_maxsymlinklen);
	ffs_oldfscompat_read(fs, VFSTOUFS(mp), sblockloc);
	UFS_LOCK(ump);
	if (fs->fs_pendingblocks != 0 || fs->fs_pendinginodes != 0) {
		log_debug("WARNING: %s: reload pending error: blocks %lld "
		    "files %d\n", fs->fs_fsmnt, (intmax_t)fs->fs_pendingblocks,
		    fs->fs_pendinginodes);
		fs->fs_pendingblocks = 0;
		fs->fs_pendinginodes = 0;
	}
	UFS_UNLOCK(ump);

	/*
	 * Step 3: re-read summary information from disk.
	 */
	size = fs->fs_cssize;
	blks = (int)howmany(size, fs->fs_fsize);
	if (fs->fs_contigsumsize > 0)
		size += fs->fs_ncg * sizeof(int32_t);
	size += fs->fs_ncg * sizeof(u_int8_t);
    free(fs->fs_csp, M_UFSMNT);
	space = malloc(size, M_UFSMNT, M_WAITOK);
	fs->fs_csp = space;
	for (i = 0; i < blks; i += fs->fs_frag) {
		size = fs->fs_bsize;
		if (i + fs->fs_frag > blks)
			size = (blks - i) * fs->fs_fsize;
		error = buf_meta_bread(devvp, fsbtodb(fs, fs->fs_csaddr + i), (int)size, NOCRED, &bp);
		if (error)
			return (error);
		bcopy((void*)buf_dataptr(bp), space, (u_int)size);
		space = (char *)space + size;
		buf_brelse(bp);
	}
	/*
	 * We no longer know anything about clusters per cylinder group.
	 */
	if (fs->fs_contigsumsize > 0) {
		fs->fs_maxcluster = lp = space;
		for (i = 0; i < fs->fs_ncg; i++)
			*lp++ = fs->fs_contigsumsize;
		space = lp;
	}
	size = fs->fs_ncg * sizeof(u_int8_t);
	fs->fs_contigdirs = (u_int8_t *)space;
	bzero(fs->fs_contigdirs, size);
	if ((flags & FFSR_UNSUSPEND) != 0) {
		bmp->mnt_kern_flag &= ~(MNTK_SUSPENDED | MNTK_SUSPEND2);
		wakeup(&bmp->mnt_flag);
	}

loop:
    vnode_iterate(mp, VNODE_RELOAD, ffs_reload_callback, &reload_iterargs);
	return (0);
}

/*
* Callback function (called by vnode_iterate).
*/
static int
ffs_reload_callback(struct vnode *vp, void *arg) {
    struct inode *ip;
    struct fs *fs;
    struct ufsmount *ump;
    struct buf *bp;
    struct {
        int error;
    } *ap = arg;
    
    ip = VTOI(vp);
    fs = ITOFS(ip);
    ump = ITOUMP(ip);
    
    // skip dead vnodes
    if (vnode_recycle(vp))
        return (VNODE_RETURNED);
    
    /*
     * Step 4: invalidate all cached file data.
     */
    if (buf_invalidateblks(vp, BUF_WRITE_DATA, 0, 0))
        panic("ffs_reload: dirty2");
    /*
     * Step 5: re-read inode data for all active vnodes.
     */
    ap->error = buf_meta_bread(ump->um_devvp, fsbtodb(fs, ino_to_fsba(fs, ip->i_number)),
                               (int)fs->fs_bsize, NOCRED, &bp);
    if (ap->error) {
        return (VNODE_RETURNED_DONE); // VNODE_RETURNED_DONE puts the vnode for us
    }
    
    if ((ap->error = ffs_load_inode(bp, ip, fs, ip->i_number)) != 0) {
        buf_brelse(bp);
        return (VNODE_RETURNED_DONE);
    }
    
    ip->i_effnlink = ip->i_nlink;
    buf_brelse(bp);
    
    return (VNODE_RETURNED);
}


/*
 * Common code for mount and mountroot
 */
static int
ffs_mountfs(struct vnode *devvp, struct mount *mp, vfs_context_t context)
{
	struct ufsmount *ump;
	struct fs *fs;
	dev_t dev;
	int error, i, len, ronly, devBlockSize;
	struct ucred *cred;
	struct mount *nmp;
    struct vfsstatfs *statfs;
	struct fsfail_task etp;
	int __unused candelete, __unused canspeedup;
	off_t loc;

	fs = NULL;
	ump = NULL;
	cred = vfs_context_ucred(context);
	ronly = (vfs_flags(mp) & MNT_RDONLY) != 0;
	dev = vnode_specrdev(devvp);
    /* get device block size */
    devBlockSize = vfs_devblocksize(mp);
    statfs = vfs_statfs(mp); log_debug("enter");
    
	if ((SBLOCKSIZE % devBlockSize) != 0) {
		error = EINVAL;
		log_debug("Invalid sectorsize %d for superblock size %d", devBlockSize, SBLOCKSIZE);
		goto out;
	}
	/* fetch the superblock and summary information */
	loc = STDSB;
	if ((vfs_flags(mp) & MNT_ROOTFS) != 0)
		loc = STDSB_NOHASHFAIL;
	if ((error = ffs_sbget(devvp, &fs, loc, M_UFSMNT, ffs_use_bread)) != 0)
		goto out;
	fs->fs_flags &= ~FS_UNCLEAN;
	if (fs->fs_clean == 0) {
		fs->fs_flags |= FS_UNCLEAN;
		if (ronly || vfs_isforce(mp) ||
		    ((fs->fs_flags & (FS_SUJ | FS_NEEDSFSCK)) == 0 &&
		     (fs->fs_flags & FS_DOSOFTDEP))) {
			log_debug("WARNING: %s was not properly dismounted\n",
			    fs->fs_fsmnt);
		} else {
			log_debug("R/W mount of %s denied. %s%s",
			    fs->fs_fsmnt, "Filesystem is not clean - run fsck.",
			    (fs->fs_flags & FS_SUJ) == 0 ? "" :
			    " Forced mount will invalidate journal contents");
			error = EPERM;
			goto out;
		}
		if ((fs->fs_pendingblocks != 0 || fs->fs_pendinginodes != 0) &&
		    vfs_isforce(mp)) {
			log_debug("WARNING: %s: lost blocks %lld files %d\n",
			    fs->fs_fsmnt, (intmax_t)fs->fs_pendingblocks,
			    fs->fs_pendinginodes);
			fs->fs_pendingblocks = 0;
			fs->fs_pendinginodes = 0;
		}
	}
	if (fs->fs_pendingblocks != 0 || fs->fs_pendinginodes != 0) {
		log_debug("WARNING: %s: mount pending error: blocks %lld "
		    "files %d\n", fs->fs_fsmnt, (intmax_t)fs->fs_pendingblocks,
		    fs->fs_pendinginodes);
		fs->fs_pendingblocks = 0;
		fs->fs_pendinginodes = 0;
	}
    // FIXME: reject gjournal filesystems
	if ((fs->fs_flags & FS_GJOURNAL) != 0) {
		log_debug("WARNING: %s: GJOURNAL flag on fs but no UFS_GJOURNAL support\n", statfs->f_mntonname);
        return EINVAL;
	}
    
	ump = malloc(sizeof(struct ufsmount), M_UFSMNT, M_WAITOK | M_ZERO);
    ump->um_compat = malloc(sizeof(struct bsdmount), M_UFSMNT, M_WAITOK | M_ZERO);
	ump->um_fs = fs;
	if (fs->fs_magic == FS_UFS1_MAGIC) {
		ump->um_fstype = UFS1;
		ump->um_balloc = ffs_balloc_ufs1;
	} else {
		ump->um_fstype = UFS2;
		ump->um_balloc = ffs_balloc_ufs2;
	}
    ump->um_ihash_lock = lck_mtx_alloc_init(ffs_lock_group, LCK_ATTR_NULL);
    ump->um_vget_critical = ialloc_critical_new();
    ump->um_valloc_critical = ialloc_critical_new();
	ump->um_blkatoff = ffs_blkatoff;
	ump->um_truncate = ffs_truncate;
	ump->um_update = ffs_update;
	ump->um_valloc = ffs_valloc;
	ump->um_vfree = ffs_vfree;
	ump->um_ifree = ffs_ifree;
	ump->um_rdonly = ffs_rdonly;
	ump->um_snapgone = ffs_snapgone;
	if ((vfs_flags(mp) & FREEBSD_MNT_UNTRUSTED) != 0)
		ump->um_check_blkno = ffs_check_blkno;
	else
		ump->um_check_blkno = NULL;
    UFS_MTX(ump) = lck_mtx_alloc_init(ffs_lock_group, LCK_ATTR_NULL);
	ffs_oldfscompat_read(fs, ump, fs->fs_sblockloc);
	fs->fs_ronly = ronly;
	fs->fs_active = NULL;
	vfs_setfsprivate(mp, ump);
	statfs->f_fsid.val[0] = fs->fs_id[0];
	statfs->f_fsid.val[1] = fs->fs_id[1];
	nmp = NULL;
	if (fs->fs_id[0] == 0 || fs->fs_id[1] == 0 ||
	    (nmp = vfs_getvfs(&statfs->f_fsid))) {
		vfs_getnewfsid(mp);
	}
	vfs_setmaxsymlen(mp, fs->fs_maxsymlinklen);
	vfs_setflags(mp, MNT_LOCAL);

    if ((fs->fs_flags & FS_MULTILABEL) != 0) {
#ifdef MAC
		MNT_ILOCK(bmp);
		vfs_flags(mp) |= FREEBSD_MNT_MULTILABEL;
		MNT_IUNLOCK(bmp);
#else
		log_debug("WARNING: %s: multilabel flag on fs but "
		    "no MAC support\n", vfs_statfs(mp)->f_mntonname);
#endif
	}
	if ((fs->fs_flags & FS_ACLS) != 0) {
#ifdef UFS_ACL
		MNT_ILOCK(bmp);

		if (vfs_flags(mp) & FREEBSD_MNT_NFS4ACLS)
			log_debug("WARNING: %s: ACLs flag on fs conflicts with "
			    "\"nfsv4acls\" mount option; option ignored\n",
			    vfs_statfs(mp)->f_mntonname);
		vfs_flags(mp) &= ~FREEBSD_MNT_NFS4ACLS;
		vfs_flags(mp) |= FREEBSD_MNT_ACLS;

		MNT_IUNLOCK(bmp);
#else
		log_debug("WARNING: %s: ACLs flag on fs but no ACLs support\n",
		    vfs_statfs(mp)->f_mntonname);
#endif
	}
	if ((fs->fs_flags & FS_NFS4ACLS) != 0) {
#ifdef UFS_ACL
		MNT_ILOCK(bmp);

		if (vfs_flags(mp) & FREEBSD_MNT_ACLS)
			log_debug("WARNING: %s: NFSv4 ACLs flag on fs conflicts "
			    "with \"acls\" mount option; option ignored\n",
			    vfs_statfs(mp)->f_mntonname);
		vfs_flags(mp) &= ~FREEBSD_MNT_ACLS;
		vfs_flags(mp) |= FREEBSD_MNT_NFS4ACLS;

		MNT_IUNLOCK(bmp);
#else
		log_debug("WARNING: %s: NFSv4 ACLs flag on fs but no "
		    "ACLs support\n", vfs_statfs(mp)->f_mntonname);
#endif
	}
	if ((fs->fs_flags & FS_TRIM) != 0) {
		len = sizeof(int);
        int features;
        if (VNOP_IOCTL(devvp, DKIOCGETFEATURES, (caddr_t)&features, 0, context) == 0) {
            if (features & DK_FEATURE_UNMAP)
                ump->um_flags |= UM_CANDELETE;
			else
				log_debug("WARNING: %s: TRIM flag on fs but disk "
                       "does not support TRIM\n",
                       vfs_statfs(mp)->f_mntonname);
		} else {
			log_debug("WARNING: %s: TRIM flag on fs but disk does "
                   "not confirm that it supports TRIM\n",
                   vfs_statfs(mp)->f_mntonname);
		}
		if (((ump->um_flags) & UM_CANDELETE) != 0) {
			ump->um_trim_tq = taskqueue_create("trim", M_WAITOK, &ump->um_trim_tq);
			ump->um_trimhash = hashinit(MAXTRIMIO, M_TRIM, &ump->um_trimlisthashsize);
		}
	}
#if 0
	len = sizeof(int);
    // TODO: looks like an artifact from the days of <5400 rpm HDDs?
	if (g_io_getattr("GEOM::canspeedup", cp, &len, &canspeedup) == 0) {
		if (canspeedup)
			ump->um_flags |= UM_CANSPEEDUP;
	}
#endif
    ump->um_flags &= ~UM_CANSPEEDUP;
	ump->um_mountp = mp;
	ump->um_dev = dev;
	ump->um_devvp = devvp;
	ump->um_nindir = fs->fs_nindir;
	ump->um_bptrtodb = fs->fs_fsbtodb;
	ump->um_seqinc = fs->fs_frag;
	for (i = 0; i < MAXQUOTAS; i++)
		ump->um_quotas[i] = NULLVP;
#ifdef UFS_EXTATTR
	ufs_extattr_uepm_init(&ump->um_extattr);
#endif
	/*
	 * Set FS local "last mounted on" information (NULL pad)
	 */
	bzero(fs->fs_fsmnt, MAXMNTLEN);
	strlcpy((char*)fs->fs_fsmnt, statfs->f_mntonname, MAXMNTLEN);
	vfs_statfs(mp)->f_iosize = fs->fs_bsize;

	if (ronly == 0) {
		fs->fs_mtime = time_seconds();
		if ((fs->fs_flags & FS_DOSOFTDEP) &&
		    (error = softdep_mount(devvp, mp, fs, context)) != 0) {
			ffs_flushfiles(mp, FORCECLOSE, context);
			goto out;
		}
//		if (fs->fs_snapinum[0] != 0)
//			ffs_snapshot_mount(mp);
		fs->fs_fmod = 1;
		fs->fs_clean = 0;
		(void) ffs_sbupdate(ump, MNT_WAIT, 0);
	}

#ifdef UFS_EXTATTR
#ifdef UFS_EXTATTR_AUTOSTART
	/*
	 *
	 * Auto-starting does the following:
	 *	- check for /.attribute in the fs, and extattr_start if so
	 *	- for each file in .attribute, enable that file with
	 * 	  an attribute of the same name.
	 * Not clear how to report errors -- probably eat them.
	 * This would all happen while the filesystem was busy/not
	 * available, so would effectively be "atomic".
	 */
	(void) ufs_extattr_autostart(mp, context);
#endif /* !UFS_EXTATTR_AUTOSTART */
#endif /* !UFS_EXTATTR */
    etp.fsid = statfs->f_fsid;
	ump->um_fsfail_task = etp;
	return (0);
out:
	if (fs != NULL) {
        free(fs->fs_csp, M_UFSMNT);
        free(fs->fs_si, M_UFSMNT);
        free(fs, M_UFSMNT);
	}
    
	if (ump) {
        if (ump->um_compat){
            if (ump->um_compat->mnt_lock){
                lck_mtx_destroy(ump->um_compat->mnt_lock, ffs_lock_group);
                lck_mtx_free(ump->um_compat->mnt_lock, ffs_lock_group);
            }
            free(ump->um_compat, M_UFSMNT);
        }

        if(ump->um_ihash_lock){
            lck_mtx_destroy(ump->um_ihash_lock, ffs_lock_group);
            lck_mtx_free(ump->um_ihash_lock, ffs_lock_group);
        }
        if (ump->um_vget_critical)
            ialloc_critical_free(ump->um_vget_critical);
        if (ump->um_valloc_critical)
            ialloc_critical_free(ump->um_valloc_critical);
        
		lck_mtx_destroy(UFS_MTX(ump), LCK_GRP_NULL);
        lck_mtx_free(UFS_MTX(ump), LCK_GRP_NULL);
        free(ump, M_UFSMNT);
		vfs_setfsprivate(mp, NULL);
	}
//    panic("debug breakpoint");
    trace_return (error);
}


/*
 * A read function for use by filesystem-layer routines.
 */
static int
ffs_use_bread(void *devfd, off_t loc, void **bufp, int size)
{
    int error, numblks;
    struct buf *bp; // 2 blocks for ufs2
    struct vnode *devvp;
    char *buf;
    enum { BLKSIZE = 512 }; // block size for the bread().
    
    ASSERT(*bufp == NULL, ("ffs_use_bread: non-NULL *bufp %p\n", bufp));
    buf = malloc(size, M_UFSMNT, M_WAITOK);
    *bufp = buf;
    numblks = size/BLKSIZE;
    devvp = (struct vnode *)devfd;
    
    for (int i = 0; i < numblks; i++) {
        if ((error = buf_meta_bread(devvp, btodb(loc, DEV_BSIZE) + i, BLKSIZE, NOCRED, &bp)) != 0){
            log_debug("failed to read superblock");
            if (bp) buf_brelse(bp);
            trace_return (error);
        }
        assert(buf_count(bp) == BLKSIZE); // make sure whole buffer wass read
        bcopy((void*)buf_dataptr(bp), buf, BLKSIZE);
        buf += BLKSIZE;
        buf_markinvalid(bp);
        buf_brelse(bp);
    }
    
    trace_return (0);
}

static int bigcgs = 0;
SYSCTL_INT(_debug, OID_AUTO, bigcgs, CTLFLAG_RW, &bigcgs, 0, "");

/*
 * Sanity checks for loading old filesystem superblocks.
 * See ffs_oldfscompat_write below for unwound actions.
 *
 * XXX - Parts get retired eventually.
 * Unfortunately new bits get added.
 */
static void
ffs_oldfscompat_read(struct fs *fs, struct ufsmount *ump, ufs2_daddr_t sblockloc)
{
	off_t maxfilesize;

	/*
	 * If not yet done, update fs_flags location and value of fs_sblockloc.
	 */
	if ((fs->fs_old_flags & FS_FLAGS_UPDATED) == 0) {
		fs->fs_flags = fs->fs_old_flags;
		fs->fs_old_flags |= FS_FLAGS_UPDATED;
		fs->fs_sblockloc = sblockloc;
	}
	/*
	 * If not yet done, update UFS1 superblock with new wider fields.
	 */
	if (fs->fs_magic == FS_UFS1_MAGIC && fs->fs_maxbsize != fs->fs_bsize) {
		fs->fs_maxbsize = fs->fs_bsize;
		fs->fs_time = fs->fs_old_time;
		fs->fs_size = fs->fs_old_size;
		fs->fs_dsize = fs->fs_old_dsize;
		fs->fs_csaddr = fs->fs_old_csaddr;
		fs->fs_cstotal.cs_ndir = fs->fs_old_cstotal.cs_ndir;
		fs->fs_cstotal.cs_nbfree = fs->fs_old_cstotal.cs_nbfree;
		fs->fs_cstotal.cs_nifree = fs->fs_old_cstotal.cs_nifree;
		fs->fs_cstotal.cs_nffree = fs->fs_old_cstotal.cs_nffree;
	}
	if (fs->fs_magic == FS_UFS1_MAGIC &&
	    fs->fs_old_inodefmt < FS_44INODEFMT) {
		fs->fs_maxfilesize = ((uint64_t)1 << 31) - 1;
		fs->fs_qbmask = ~fs->fs_bmask;
		fs->fs_qfmask = ~fs->fs_fmask;
	}
	if (fs->fs_magic == FS_UFS1_MAGIC) {
		ump->um_savedmaxfilesize = fs->fs_maxfilesize;
		maxfilesize = (uint64_t)0x80000000 * fs->fs_bsize - 1;
		if (fs->fs_maxfilesize > maxfilesize)
			fs->fs_maxfilesize = maxfilesize;
	}
	/* Compatibility for old filesystems */
	if (fs->fs_avgfilesize <= 0)
		fs->fs_avgfilesize = AVFILESIZ;
	if (fs->fs_avgfpdir <= 0)
		fs->fs_avgfpdir = AFPDIR;
	if (bigcgs) {
		fs->fs_save_cgsize = fs->fs_cgsize;
		fs->fs_cgsize = fs->fs_bsize;
	}
}

/*
 * Unwinding superblock updates for old filesystems.
 * See ffs_oldfscompat_read above for details.
 *
 * XXX - Parts get retired eventually.
 * Unfortunately new bits get added.
 */
void
ffs_oldfscompat_write(struct fs *fs, struct ufsmount *ump)
{

	/*
	 * Copy back UFS2 updated fields that UFS1 inspects.
	 */
	if (fs->fs_magic == FS_UFS1_MAGIC) {
		fs->fs_old_time = (int) fs->fs_time;
		fs->fs_old_cstotal.cs_ndir = (int) fs->fs_cstotal.cs_ndir;
		fs->fs_old_cstotal.cs_nbfree = (int) fs->fs_cstotal.cs_nbfree;
		fs->fs_old_cstotal.cs_nifree = (int) fs->fs_cstotal.cs_nifree;
		fs->fs_old_cstotal.cs_nffree = (int) fs->fs_cstotal.cs_nffree;
		fs->fs_maxfilesize = ump->um_savedmaxfilesize;
	}
	if (bigcgs) {
		fs->fs_cgsize = fs->fs_save_cgsize;
		fs->fs_save_cgsize = 0;
	}
}

extern int hz;
int tsleep(void *chan, int pri, const char *wmsg, int timo);

/*
 * unmount system call
 */
int
ffs_unmount(struct mount *mp, int mntflags, struct vfs_context *context)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct fs *fs;
	int error, flags, susp;
#ifdef UFS_EXTATTR
	int e_restart;
#endif

	flags = 0;
	fs = ump->um_fs;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	susp = fs->fs_ronly == 0;
#ifdef UFS_EXTATTR
	if ((error = ufs_extattr_stop(mp, td))) {
		if (error != EOPNOTSUPP)
			log_debug("WARNING: unmount %s: ufs_extattr_stop "
			    "returned errno %d\n", vfs_statfs(mp)->f_mntonname,
			    error);
		e_restart = 0;
	} else {
		ufs_extattr_uepm_destroy(&ump->um_extattr);
		e_restart = 1;
	}
#endif
	if (susp) {
		error = vfs_write_suspend_umnt(mp);
		if (error != 0)
			goto fail1;
	}
	if (MOUNTEDSOFTDEP(mp))
		error = softdep_flushfiles(mp, flags, context);
	else
		error = ffs_flushfiles(mp, flags, context);
	if (error != 0 && !ffs_fsfail_cleanup(ump, error))
		goto fail;

	UFS_LOCK(ump);
	if (fs->fs_pendingblocks != 0 || fs->fs_pendinginodes != 0) {
		log_debug("WARNING: unmount %s: pending error: blocks %lld "
		    "files %d\n", fs->fs_fsmnt, (intmax_t)fs->fs_pendingblocks,
		    fs->fs_pendinginodes);
		fs->fs_pendingblocks = 0;
		fs->fs_pendinginodes = 0;
	}
	UFS_UNLOCK(ump);
	if (MOUNTEDSOFTDEP(mp))
		softdep_unmount(mp);
	if (fs->fs_ronly == 0 || ump->um_fsckpid > 0) {
		fs->fs_clean = fs->fs_flags & (FS_UNCLEAN|FS_NEEDSFSCK) ? 0 : 1;
		error = ffs_sbupdate(ump, MNT_WAIT, 0);
		if (ffs_fsfail_cleanup(ump, error))
			error = 0;
		if (error != 0 && !ffs_fsfail_cleanup(ump, error)) {
			fs->fs_clean = 0;
			goto fail;
		}
	}
	if (susp)
		vfs_write_resume(mp, VR_START_WRITE);
    
	if (ump->um_trim_tq != NULL) {
        while (ump->um_trim_inflight != 0)
            tsleep(NULL, PINOD, "ufsutr", hz);
        taskqueue_drain_all(ump->um_trim_tq);
        taskqueue_free(ump->um_trim_tq);
        hashdestroy(ump->um_trimhash, M_TEMP, ump->um_trimlisthashsize);
    }
    
    
	if (ump->um_fsckpid > 0) {
		/*
		 * Return to normal read-only mode.
		 */
		ump->um_fsckpid = 0;
	}
    
    ialloc_critical_free(ump->um_vget_critical);
    ialloc_critical_free(ump->um_valloc_critical);
    lck_mtx_destroy(ump->um_ihash_lock, ffs_lock_group);
    lck_mtx_free(ump->um_ihash_lock, ffs_lock_group);
	lck_mtx_destroy(UFS_MTX(ump), ffs_lock_group);
    lck_mtx_free(UFS_MTX(ump), ffs_lock_group);
    free(fs->fs_csp, M_UFSMNT);
    free(fs->fs_si, M_UFSMNT);
    free(fs, M_UFSMNT);
    free(ump->um_compat, M_UFSMNT);
    free(ump, M_UFSMNT);
    vfs_setfsprivate(mp, NULL);
    
	return (error);

fail:
	if (susp)
		vfs_write_resume(mp, VR_START_WRITE);
fail1:
#ifdef UFS_EXTATTR
	if (e_restart) {
		ufs_extattr_uepm_init(&ump->um_extattr);
#ifdef UFS_EXTATTR_AUTOSTART
		(void) ufs_extattr_autostart(mp, td);
#endif
	}
#endif

	return (error);
}

/*
 * Flush out all the files in a filesystem.
 */
int
ffs_flushfiles(struct mount *mp, int flags, vfs_context_t context)
{
	struct ufsmount *ump;
    struct bsdmount *bmp;
	int qerror, error;

	ump = VFSTOUFS(mp);
    bmp = ump->um_compat;
	qerror = 0;
#ifdef QUOTA
	if (vfs_flags(mp) & FREEBSD_MNT_QUOTA) {
		int i;
		error = vflush(mp, 0, SKIPSYSTEM|flags, td);
		if (error)
			return (error);
		for (i = 0; i < MAXQUOTAS; i++) {
			error = quotaoff(td, mp, i);
			if (error != 0) {
				if ((flags & EARLYFLUSH) == 0)
					return (error);
				else
					qerror = error;
			}
		}

		/*
		 * Here we fall through to vflush again to ensure that
		 * we have gotten rid of all the system vnodes, unless
		 * quotas must not be closed.
		 */
	}
#endif
	if (bmp->mnt_kern_flag & MNTK_VVCOPYONWRITE) {
		if ((error = vflush(mp, 0, SKIPSYSTEM | flags)) != 0)
			return (error);
		ffs_snapshot_unmount(mp);
		flags |= FORCECLOSE;
		/*
		 * Here we fall through to vflush again to ensure
		 * that we have gotten rid of all the system vnodes.
		 */
	}

	/*
	 * Do not close system files if quotas were not closed, to be
	 * able to sync the remaining dquots.  The freeblks softupdate
	 * workitems might hold a reference on a dquot, preventing
	 * quotaoff() from completing.  Next round of
	 * softdep_flushworklist() iteration should process the
	 * blockers, allowing the next run of quotaoff() to finally
	 * flush held dquots.
	 *
	 * Otherwise, flush all the files.
	 */
	if (qerror == 0 && (error = vflush(mp, 0, flags)) != 0)
		return (error);

	/*
	 * Flush filesystem metadata.
	 */
	error = VNOP_FSYNC(ump->um_devvp, MNT_WAIT, context);
	return (error);
}

static bool
sync_doupdate(struct inode *ip)
{

	return ((ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) != 0);
}

static int
ffs_sync_lazy_cb(struct vnode *vp, void *arg)
{
    struct inode *ip;
    struct ffs_cbargs *ap;
    int error;
    
    ip = VTOI(vp);
    ap = arg;
    error = 0;
    
    /*
     * The IN_ACCESS flag is converted to IN_MODIFIED by
     * ufs_vnop_close() and ufs_getattr() by the calls to
     * ufs_itimes_locked(), without subsequent UFS_UPDATE().
     * Test also all the other timestamp flags too, to pick up
     * any other cases that could be missed.
     */
    if (!sync_doupdate(ip) && (ip->i_viflag & VI_NEEDINACT) == 0) {
        return VNODE_RETURNED;
    }

#ifdef QUOTA
    qsyncvp(vp);
#endif

    if (sync_doupdate(ip)){
        error = ffs_update(vp, 0);
    }
    
    if (error){
        ap->error = error;
    }
    
    return VNODE_RETURNED;
}

/*
 * For a lazy sync, we only care about access times, quotas and the
 * superblock.  Other filesystem changes are already converted to
 * cylinder group blocks or inode blocks updates and are written to
 * disk by syncer.
 */
static int
ffs_sync_lazy(
     struct mount *mp,
    struct vfs_context *context)
{
	int error = 0;
    struct ffs_cbargs args;

	if ((vfs_flags(mp) & MNT_NOATIME) != 0) {
#ifdef QUOTA
		qsync(mp);
#endif
		goto sbupdate;
	}
    
    args.error = 0;
    args.context = context;
    vnode_iterate(mp, VNODE_NODEAD /*| VNODE_WITHID*/, ffs_sync_lazy_cb, &args);
    
    if((error = args.error) != 0){
        return error;
    }
    
sbupdate:
    if (VFSTOUFS(mp)->um_fs->fs_fmod != 0){
        error = ffs_sbupdate(VFSTOUFS(mp), MNT_LAZY, 0);
    }
    
	return (error);
}

static int
ffs_sync_cb(struct vnode *vp, void *arg)
{
    struct ffs_cbargs *ap;
    struct inode *ip;
    
    ap = arg;
    ip = VTOI(vp);
    
    /*
     * Depend on the vnode interlock to keep things stable enough
     * for a quick test.  Since there might be hundreds of
     * thousands of vnodes, we cannot afford even a subroutine
     * call unless there's a good chance that we have work to do.
     */
    if (vnode_isrecycled(vp)) {
        return VNODE_RETURNED;
    }

    if ((ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) == 0 &&
        !vnode_hasdirtyblks(vp)) {
        // if the inode doesn't have any modified data, skip over it.
        // TODO: Not sure if this should be done though...
        return VNODE_RETURNED;
    }

#ifdef QUOTA
    qsyncvp(vp);
#endif

    /*
     * Sync the inode...
     */
    for (;;) {
        ap->error = ffs_syncvnode(vp, ap->waitfor, 0);
        if (ap->error == ERECYCLE) // TODO: check out this return value from ffs_syncvnode()
            continue;
        break;
    }
    return VNODE_RETURNED;
}


/*
 * Go through the disk queues to initiate sandbagged IO;
 * go through the inodes to write those that have been modified;
 * initiate the writing of the super block if it has been modified.
 *
 * Note: we are always called with the filesystem marked busy using
 * vfs_busy_bsd().
 */
__XNU_PRIVATE_EXTERN int
ffs_sync(struct mount *mp, int waitfor, vfs_context_t context)
{
	struct vnode *devvp;
    struct ufsmount *ump;
    struct bsdmount *bmp;
    struct ffs_cbargs args;
	struct fs *fs;
	int error, count, lockreq, allerror = 0;
	int suspend;
	int suspended;
	int secondary_writes;
	int secondary_accwrites;
	int softdep_deps;
	int softdep_accdeps;
    
    ump = VFSTOUFS(mp);
    bmp = ump->um_compat;
    devvp = ump->um_devvp;
    fs = ump->um_fs;
	suspend = 0;
	suspended = 0;
    
	if (fs->fs_fmod != 0 && fs->fs_ronly != 0 && ump->um_fsckpid == 0)
		panic("%s: ffs_sync: modification on read-only filesystem",
		    fs->fs_fsmnt);
	if (waitfor == MNT_LAZY) {
        return (ffs_sync_lazy(mp, context));
	}

	/*
	 * Write back each (modified) inode.
	 */
	lockreq = UFS_LOCK_EXCLUSIVE | UFS_LOCK_NOWAIT;
	if (waitfor == MNT_SUSPEND) {
		suspend = 1;
		waitfor = MNT_WAIT;
	}
	if (waitfor == MNT_WAIT)
		lockreq = UFS_LOCK_EXCLUSIVE;
	lockreq |= UFS_LOCK_INTERLOCK | UFS_LOCK_SLEEPFAIL;
loop:
	/* Grab snapshot of secondary write counts */
	MNT_ILOCK(bmp);
	secondary_writes = bmp->mnt_secondary_writes;
	secondary_accwrites = bmp->mnt_secondary_accwrites;
	MNT_IUNLOCK(bmp);

	/* Grab snapshot of softdep dependency counts */
	softdep_get_depcounts(mp, &softdep_deps, &softdep_accdeps);

    args.error = 0;
    args.context = context;
    args.waitfor = (waitfor == MNT_WAIT);
    vnode_iterate(mp, VNODE_NODEAD | VNODE_WITHID, ffs_sync_cb, &args); // return code isn't important
    error = args.error; // set error if any

	/*
	 * Force stale filesystem control information to be flushed.
	 */
	if (waitfor == MNT_WAIT) {
		if ((error = softdep_flushworklist(ump->um_mountp, &count, context)))
			allerror = error;
		if (ffs_fsfail_cleanup(ump, allerror))
			allerror = 0;
		/* Flushed work items may create new vnodes to clean */
		if (allerror == 0 && count)
			goto loop;
	}

    /* Flush all dirty buffers on our special device. */
    error = VNOP_FSYNC(devvp, waitfor, context);
    if (MOUNTEDSOFTDEP(mp) && (error == 0 || error == EAGAIN))
        error = ffs_sbupdate(ump, waitfor, 0);
    if (error != 0)
        allerror = error;
    if (ffs_fsfail_cleanup(ump, allerror))
        allerror = 0;
    if (allerror == 0 && waitfor == MNT_WAIT)
        goto loop;

    if (suspend != 0) {
		if (softdep_check_suspend(mp,
					  devvp,
					  softdep_deps,
					  softdep_accdeps,
					  secondary_writes,
					  secondary_accwrites) != 0) {
			MNT_IUNLOCK(bmp);
			goto loop;	/* More work needed */
		}
		bmp->mnt_kern_flag |= MNTK_SUSPEND2 | MNTK_SUSPENDED;
		MNT_IUNLOCK(bmp);
		suspended = 1;
	}

	/*
	 * Write back modified superblock.
	 */
	if (fs->fs_fmod != 0 &&
	    (error = ffs_sbupdate(ump, waitfor, suspended)) != 0)
		allerror = error;
	if (ffs_fsfail_cleanup(ump, allerror))
		allerror = 0;
	return (allerror);
}

int
VFS_SYNC(struct mount *mp, int waitfor)
{
    return ffs_sync(mp, waitfor, vfs_context_current());
}

int
ffs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, vfs_context_t context)
{
    struct vfs_vget_args args = {0};
	return (ffs_vgetf(mp, (ino_t)ino, &args, vpp, context));
}

int
VFS_VGET(struct mount *mp, ino_t ino, struct vfs_vget_args *vap, struct vnode **vpp, vfs_context_t context)
{
    return ffs_vgetf(mp, ino, vap, vpp, context);
}

static void vget_rel(struct inode *ip){
    ufs_hash_remove(ip);
    inode_wakeup(ip, INL_WAIT_ALLOC, INL_ALLOC);
    if (ip->i_lock) {
        inode_lock_free(ip);
    }
    FREE(ip, M_UFSMNT);
}

int
ffs_vgetf(struct mount *mp, ino_t ino, struct vfs_vget_args *ap,
          struct vnode **vpp, vfs_context_t context)
{
	struct fs *fs;
	struct inode *ip;
	struct ufsmount *ump;
	struct buf *bp;
	struct vnode *vp;
	daddr_t dbn;
	int error;
    struct vnode_init_args args = {0};
    int flags, ffs_flags;
    
    ump = VFSTOUFS(mp);
    fs = ump->um_fs;
    flags = ap ? ap->flags : 0;
    ffs_flags = ap ? ap->flags : 0;
    
    MPASS((ffs_flags & FFSV_REPLACE) == 0 || (flags & UFS_LOCK_EXCLUSIVE) != 0);
    
restart:
    if ((error = ufs_hash_get(ump, ino, 0, vpp)) != 0)
        trace_return (error);
	if (*vpp != NULL) {
		if ((ffs_flags & FFSV_REPLACE) == 0)
			return (0);
		vnode_put(*vpp);
	}
    
	/*
     * Lock out the creation of new entries in the hash table in
     * case getnewvnode() or MALLOC() blocks, otherwise a duplicate
     * may occur!
     */
    if (ialloc_is_critical(ump->um_vget_critical, ino)) {
        ialloc_critical_wait(ump->um_vget_critical, ino);
        goto restart;
    }
    
    ialloc_critical_enter(ump->um_vget_critical, ino);
    ip = malloc(sizeof(struct inode), 0,  M_WAITOK | M_ZERO);
    if (ip == NULL) {
        ialloc_critical_leave(ump->um_vget_critical, ino);
        trace_return (ENOMEM);
    }
    
    SET(ip->i_lflags, INL_ALLOC);
    ip->i_lock = inode_lock_new();
    ip->i_ump = ump;
    ip->i_number = ino;
    ip->i_ea_refs = 0;
    ip->i_nextclustercg = -1;
    ip->i_flag = fs->fs_magic == FS_UFS1_MAGIC ? 0 : IN_UFS2;
    ip->i_mode = 0; /* ensure error cases below throw away vnode */
#ifdef DIAGNOSTIC
    ufs_init_trackers(ip);
#endif
#ifdef QUOTA
    {
        int i;
        for (i = 0; i < MAXQUOTAS; i++)
            ip->i_dquot[i] = NODQUOT;
    }
#endif
    ufs_hash_insert(ip);
    ialloc_critical_leave(ump->um_vget_critical, ino);
    
	/* Read in the disk contents for the inode, copy into the inode. */
	dbn = fsbtodb(fs, ino_to_fsba(fs, ino));
	error = ffs_meta_bread(ump, ump->um_devvp, dbn, fs->fs_bsize, NOCRED, 0, NULL, &bp);
	if (error != 0) {
		/*
		 * The inode does not contain anything useful, so it would
		 * be misleading to leave it on its hash chain. With mode
		 * still zero, it will be unlinked and returned to the free
		 * list by vnode_put().
		 */
        if (bp) buf_brelse(bp);
        vget_rel(ip);
        *vpp = NULL;
		return (error);
	}
    
	if (I_IS_UFS1(ip))
		ip->i_din1 = malloc(sizeof(struct ufs1_dinode), 0, M_WAITOK);
	else
		ip->i_din2 = malloc(sizeof(struct ufs2_dinode), 0, M_WAITOK);
    
    error = ffs_load_inode(bp, ip, fs, ino);
    buf_brelse(bp);
    
    if (error != 0) {
        vget_rel(ip);
		*vpp = NULL;
		return (error);
	}
    
    args.vi_cnp = ap ? ap->cnp : NULL;
    args.vi_ctx = context;
    args.vi_flags = flags;
    args.vi_ip = ip;
    args.vi_parent = ap ? ap->dvp : NULL;
    args.vi_system_file = false;

	/*
	 * Initialize the vnode from the inode, check for aliases.
	 * Note that the underlying vnode may have changed.
	 */
    /* TODO: make sure ffs_snapshot is able to set args.vi_system_node through a flag */
	error = ufs_getnewvnode(mp, &args, &vp);
	if (error) {
        vget_rel(ip);
		*vpp = NULL;
		return (error);
	}
    
    if (DOINGSOFTDEP(vp))
        softdep_load_inodeblock(ip);
    else
        ip->i_effnlink = ip->i_nlink;
    
	/*
	 * Finish inode initialization.
	 */
//	if (vnode_vtype(vp) != VFIFO) {
//		/* FFS supports shared locking for all files except fifos. */
//		VN_LOCK_ASHARE(vp);
//	}
    
	/*
	 * Set up a generation number for this inode if it does not
	 * already have one. This should only happen on old filesystems.
	 */
	if (ip->i_gen == 0) {
		while (ip->i_gen == 0)
			ip->i_gen = random();
		if (!vfs_isrdonly(mp)) {
			UFS_INODE_SET_FLAG(ip, IN_MODIFIED);
			DIP_SET(ip, i_gen, (int)ip->i_gen);
		}
	}
#ifdef MAC
	if ((vfs_flags(mp) & MNT_MULTILABEL) && ip->i_mode) {
		/*
		 * If this vnode is already allocated, and we're running
		 * multi-label, attempt to perform a label association
		 * from the extended attributes on the inode.
		 */
		error = mac_vnode_associate_extattr(mp, vp);
		if (error) {
			/* ufs_inactive will release ip->i_devvp ref. */
			vn_revoke(vp, 1, context);
			vnode_put(vp);
			*vpp = NULL;
			return (error);
		}
	}
#endif
    inode_wakeup(ip, INL_WAIT_ALLOC, INL_ALLOC);
	*vpp = vp;
	trace_return (0);
}


/*
 * Vnode pointer to File handle
 */
int
ffs_vptofh(struct vnode *vp, int *fhlen, unsigned char *fhp, vfs_context_t context)
{
    struct inode *ip;
    struct ufid *ufhp;

    ip = VTOI(vp);
    ufhp = (struct ufid *)fhp;
    *fhlen = ufhp->ufid_len = sizeof(struct ufid);
    ufhp->ufid_ino = ip->i_number;
    ufhp->ufid_gen = (int)ip->i_gen;
    return (0);
}


/*
 * File handle to vnode
 *
 * Have to be really careful about stale file handles:
 * - check that the inode number is valid
 * - for UFS2 check that the inode number is initialized
 * - call ffs_vget() to get the locked inode
 * - check for an unallocated inode (i_mode == 0)
 * - check that the given client host has export rights and return
 *   those rights via. exflagsp and credanonp
 */
int
ffs_fhtovp(struct mount *mp, int fhlen, unsigned char *fhp,
           struct vnode **vpp, struct vfs_context *context)
{
	struct ufid *ufhp;
	struct ufsmount *ump;
	struct fs *fs;
	struct cg *cgp;
	struct buf *bp;
	ino_t ino;
	u_int cg;
	int error;

	ufhp = (struct ufid *)fhp;
	ino = ufhp->ufid_ino;
	ump = VFSTOUFS(mp);
	fs = ump->um_fs;
	if (ino < UFS_ROOTINO || ino >= fs->fs_ncg * fs->fs_ipg)
		return (ESTALE);
	/*
	 * Need to check if inode is initialized because UFS2 does lazy
	 * initialization and nfs_fhtovp can offer arbitrary inode numbers.
	 */
	if (fs->fs_magic != FS_UFS2_MAGIC)
		return (ufs_fhtovp(mp, sizeof(struct ufid), ufhp, vpp, context));
	cg = ino_to_cg(fs, ino);
	if ((error = ffs_getcg(fs, ump->um_devvp, cg, 0, &bp, &cgp)) != 0)
		return (error);
	if (ino >= cg * fs->fs_ipg + cgp->cg_initediblk) {
		buf_brelse(bp);
		return (ESTALE);
	}
	buf_brelse(bp);
	return (ufs_fhtovp(mp, sizeof(struct ufid), ufhp, vpp, context));
}

/*
 * Initialize the filesystem.
 */
int
ffs_init(struct vfsconf *vfsp)
{
    log_debug("enter");
	softdep_initialize();
	trace_return (ufs_init(vfsp));
}

/*
 * Undo the work of ffs_init().
 */
int
ffs_uninit(struct vfsconf *vfsp)
{
    log_debug("enter");
	ufs_uninit(vfsp);
	softdep_uninitialize();
    log_debug("Unloading ufsX driver");
	return 0;
}

/*
 * Structure used to pass information from ffs_sbupdate to its
 * helper routine ffs_use_bwrite.
 */
struct devfd {
	struct ufsmount	*ump;
	struct buf	    *sbbp;
	int		         waitfor;
	int		         suspended;
	int		         error;
};

/*
 * Write a superblock and associated information back to disk.
 */
int
ffs_sbupdate(struct ufsmount *ump, int waitfor, int suspended)
{
	struct fs *fs;
	struct buf *sbbp;
	struct devfd devfd;

	fs = ump->um_fs;
	if (fs->fs_ronly == 1 &&
	    (vfs_flags(ump->um_mountp) & (MNT_RDONLY | MNT_UPDATE)) !=
	    (MNT_RDONLY | MNT_UPDATE) && ump->um_fsckpid == 0)
		panic("ffs_sbupdate: write read-only filesystem");
	/*
	 * We use the superblock's buf to serialize calls to ffs_sbupdate().
	 */
	sbbp = getblk(ump->um_devvp, btodb(fs->fs_sblockloc, ump->um_devbsize),
                      (int)fs->fs_sbsize, 0, 0, BLK_META);
	/*
	 * Initialize info needed for write function.
	 */
	devfd.ump = ump;
	devfd.sbbp = sbbp;
	devfd.waitfor = waitfor;
	devfd.suspended = suspended;
	devfd.error = 0;
	return (ffs_sbput(&devfd, fs, fs->fs_sblockloc, ffs_use_bwrite));
}

/*
 * Write function for use by filesystem-layer routines.
 */
static int
ffs_use_bwrite(void *devfd, off_t loc, void *buf, int size)
{
	struct devfd *devfdp;
	struct ufsmount *ump;
	struct buf *bp;
	struct fs *fs;
	int error;

	devfdp = devfd;
	ump = devfdp->ump;
	fs = ump->um_fs;
	/*
	 * Writing the superblock summary information.
     * We won't use our custom buf_* functions for this.
     * By doing so, we automatically assert the bp == B_VALIDSUSPWRT.
	 */
	if (loc != fs->fs_sblockloc) {
		bp = buf_getblk(ump->um_devvp, btodb(loc, ump->um_devbsize), size, 0, 0, BLK_META);
		bcopy(buf, (void*)buf_dataptr(bp), (u_int)size);
		if (devfdp->suspended)
            buf_setflags(bp, 0);
		if (devfdp->waitfor != MNT_WAIT)
			buf_bawrite(bp);
		else if ((error = buf_bwrite(bp)) != 0)
			devfdp->error = error;
		return (0);
	}
	/*
	 * Writing the superblock itself. We need to do special checks for it.
	 */
	bp = devfdp->sbbp;
	if (ffs_fsfail_cleanup(ump, devfdp->error))
		devfdp->error = 0;
	if (devfdp->error != 0) {
		buf_brelse(bp);
		return (devfdp->error);
	}
	if (fs->fs_magic == FS_UFS1_MAGIC && fs->fs_sblockloc != SBLOCK_UFS1 &&
	    (fs->fs_old_flags & FS_FLAGS_UPDATED) == 0) {
		log_debug("WARNING: %s: correcting fs_sblockloc from %lld to %d\n",
		    fs->fs_fsmnt, fs->fs_sblockloc, SBLOCK_UFS1);
		fs->fs_sblockloc = SBLOCK_UFS1;
	}
	if (fs->fs_magic == FS_UFS2_MAGIC && fs->fs_sblockloc != SBLOCK_UFS2 &&
	    (fs->fs_old_flags & FS_FLAGS_UPDATED) == 0) {
		log_debug("WARNING: %s: correcting fs_sblockloc from %lld to %d\n",
		    fs->fs_fsmnt, fs->fs_sblockloc, SBLOCK_UFS2);
		fs->fs_sblockloc = SBLOCK_UFS2;
	}
	if (MOUNTEDSOFTDEP(ump->um_mountp))
		softdep_setup_sbupdate(ump, (struct fs *)buf_dataptr(bp), bp);
	bcopy((caddr_t)fs, (caddr_t)buf_dataptr(bp), (u_int)fs->fs_sbsize);
	fs = (struct fs *)buf_dataptr(bp);
	ffs_oldfscompat_write(fs, ump);
	fs->fs_si = NULL;
	/* Recalculate the superblock hash */
	fs->fs_ckhash = ffs_calc_sbhash(fs);
	if (devfdp->suspended)
		buf_setflags(bp, 0);//B_VALIDSUSPWRT);
	if (devfdp->waitfor != MNT_WAIT)
		buf_bawrite(bp);
	else if ((error = buf_bwrite(bp)) != 0)
		devfdp->error = error;
	return (devfdp->error);
}

static void
ffs_ifree(struct ufsmount *ump, struct inode *ip)
{
	if (ump->um_fstype == UFS1 && ip->i_din1 != NULL)
        free(ip->i_din1, M_TEMP);
	else if (ip->i_din2 != NULL)
        free(ip->i_din1, M_TEMP);
    free(ip, M_TEMP);
}

static int dobkgrdwrite = 1;
SYSCTL_INT(_debug, OID_AUTO, dobkgrdwrite, CTLFLAG_RW, &dobkgrdwrite, 0,
           "Do background writes (honoring the BV_BKGRDWRITE flag)?");

/*
 * Complete a background write started from bwrite.
 */
static void
ffs_backgroundwritedone(struct buf *bp, void* arg)
{
	struct buf *origbp;
    struct bufpriv *bpriv;
    struct bufpriv *origbpriv;

    /*
     * we pass the original buffer through the arg instead of looking for it
     */
    if ((origbp = arg) == NULL)
        panic("backgroundwritedone: lost buffer");
    
    bpriv = (struct bufpriv *)buf_fsprivate(bp);
    origbpriv = (struct bufpriv *)buf_fsprivate(origbp);
    
#ifdef SOFTUPDATES
	if (!LIST_EMPTY(&bpriv->b_dep) && buf_error(bp) != 0)
		softdep_handle_error(bp);
#endif
    
	/*
	 * We should mark the cylinder group buffer origbp as
	 * dirty, to not lose the failed write.
	 */
    if (buf_error(bp)){
        // same as marking the buffer delayed.
        // just mark the origignal buffer as delayed
//        origbpriv->b_vflags = BV_BKGRDERR;
//        buf_markdelayed(origbp);
    }
	/*
	 * Process dependencies then return any unfinished ones.
	 */
#ifdef SOFTUPDATES
	if (!LIST_EMPTY(&bpriv->b_dep) && buf_error(bp) == 0)
		buf_complete(bp);
	if (!LIST_EMPTY(&bpriv->b_dep))
		softdep_move_dependencies(bp, origbp);
#endif
	/*
	 * This buffer is marked B_NOCACHE so when it is released
	 * by biodone it will be tossed.
	 */
    buf_markclean(bp); // make sure the buf isn't marked with a delayed write
	buf_setflags(bp, B_NOCACHE); // makes the buffer uncached
    buf_setvnode(bp, NULL);

	/*
	 * Prevent buf_brelse() from trying to keep and re-dirtying bp on
	 * errors. It causes b_bufobj dereference in
	 * bdirty()/reassignbuf(), and b_bufobj was cleared in
	 * pbrelvp() above.
	 */
    
    if (buf_error(bp) != 0){
        buf_markinvalid(bp);
        if (buf_valid(bp)){
            buf_biodone(bp);
        }
    }
	/*
	 * Clear the BV_BKGRDINPROG flag in the original buffer
	 * and awaken it if it is waiting for the write to complete.
	 * If BV_BKGRDINPROG is not set in the original buffer it must
	 * have been released and re-instantiated - which is not legal.
	 */
	ASSERT((origbpriv->b_vflags & BV_BKGRDINPROG), ("backgroundwritedone: lost buffer2"));
	origbpriv->b_vflags &= ~BV_BKGRDINPROG;
	if (origbpriv->b_vflags & BV_BKGRDWAIT) {
		origbpriv->b_vflags &= ~BV_BKGRDWAIT;
		wakeup(&origbpriv->b_xflags);
	}
}

/*
 * Write, release buffer on completion.  (Done by iodone
 * if async).  Do not bother writing anything if the buffer
 * is invalid.
 *
 * Note that we set B_CACHE here, indicating that buffer is
 * fully valid and thus cacheable.  This is true even of NFS
 * now so we set it generally.  This could be set either here
 * or in biodone() since the I/O is synchronous.  We put it
 * here.
 */
int
ffs_bufwrite(struct buf *bp)
{
	struct buf *newbp;
    struct bufpriv *bpriv;
    struct bufpriv *newbpriv;
	struct cg *cgp;


	if (!buf_valid(bp)) {
		buf_brelse(bp);
		return (0);
	}
    
    bpriv = buf_fsprivate(bp);
    newbpriv = buf_fsprivate(bp);

	if ((buf_flags(bp) & B_LOCKED) == 0)
		panic("bufwrite: buffer is not busy???");
	/*
	 * If a background write is already in progress, delay
	 * writing this block if it is asynchronous. Otherwise
	 * wait for the background write to complete.
	 */
	if (bpriv->b_vflags & BV_BKGRDINPROG) {
		if (buf_flags(bp) & B_ASYNC) {
			buf_bdwrite(bp);
			return (0);
		}
		bpriv->b_vflags |= BV_BKGRDWAIT;
		msleep(&bpriv->b_xflags, NULL, PRIBIO, "bwrbg", 0); // FIXME: NULL pointer...
		if (bpriv->b_vflags & BV_BKGRDINPROG)
			panic("bufwrite: still writing");
	}
	bpriv->b_vflags &= ~BV_BKGRDERR;

	/*
	 * If this buffer is marked for background writing and we
	 * do not have to wait for it, make a copy and write the
	 * copy so as to leave this buffer ready for further use.
	 *
	 * This optimization eats a lot of memory.  If we have a page
	 * or buffer shortfall we can't do it.
	 */
	if (dobkgrdwrite && (buf_xflags(bp) & BX_BKGRDWRITE) && (buf_flags(bp) & B_ASYNC)) {
		ASSERT(buf_callback(bp) == NULL,
               ("bufwrite: needs chained iodone (%p)", buf_callback(bp)));

        /* clone the block.. */
        /* We provide the original buffer as our primary argument in the callback */
        newbp = buf_clone(bp, 0, buf_size(bp), ffs_backgroundwritedone, bp);
		if (newbp == NULL)
			goto normal_write;
        
        extern struct bufpriv* bufpriv_create(struct buf *bp);
        newbpriv = bufpriv_create(newbp);
        
        buf_setflags(bp, B_PASSIVE); // mark background io.
		newbpriv->b_xflags |= (buf_xflags(bp) & BX_FSPRIV) | BX_BKGRDMARKER;
        buf_setlblkno(newbp, buf_lblkno(bp));
		buf_setblkno(newbp, buf_blkno(bp));
		buf_setflags(newbp, B_ASYNC);
        buf_setvnode(newbp, buf_vnode(bp));

#ifdef SOFTUPDATES
		/*
		 * Move over the dependencies.  If there are rollbacks,
		 * leave the parent buffer dirtied as it will need to
		 * be written again.
		 */

		if (LIST_EMPTY(&bpriv->b_dep) ||
		    softdep_move_dependencies(bp, newbp) == 0)
			buf_markclean(bp);
#else
//		buf_markdelayed(bp); // keeps OG buffer from being recycled until the copy is written
#endif

		/*
		 * Initiate write on the copy, release the original.  The
		 * B_DELWRI flag prevents it from going away until
		 * the background write completes. We have to recalculate
		 * its check hash in case the buffer gets freed and then
		 * reconstituted from the buffer cache during a later read.
		 */
		if ((buf_xflags(bp) & BX_CYLGRP) != 0) {
			cgp = (struct cg *)buf_dataptr(bp);
			cgp->cg_ckhash = 0;
			cgp->cg_ckhash = calculate_crc32c(~0L, (const unsigned char*)buf_dataptr(bp), buf_count(bp));
		}
		buf_brelse(bp);
		bp = newbp;
	} else
		/* Mark the buffer clean */
		buf_markclean(bp);

	/* Let the normal bufwrite do the rest for us */
normal_write:
	/*
	 * If we are writing a cylinder group, update its time.
	 */
	if ((buf_xflags(bp) & BX_CYLGRP) != 0) {
		cgp = (struct cg *)buf_dataptr(bp);
		cgp->cg_old_time = cgp->cg_time = (int)time_seconds();
	}
	return (buf_bwrite(bp));
}

int ffs_ioctl(struct mount *mp, unsigned long command,
              caddr_t data, int flags, struct vfs_context *context)
{
    int error = ENOTSUP;
    return (error);
}

static void
ffs_geom_strategy(struct bufobj *bo, struct buf *bp)
{
	struct vnode *vp;
    struct inode *ip;
    struct mount *mp;
//	struct buf *tbp;
    struct ufsmount *ump;
//	int error, nocopy, bflags;
//
//	/*
//	 * This is the bufobj strategy for the private VCHR vnodes
//	 * used by FFS to access the underlying storage device.
//	 * We override the default bufobj strategy and thus bypass
//	 * VOP_STRATEGY() for these vnodes.
//	 */
	vp = buf_vnode(bp);
    mp = vnode_mount(vp);
    ump = VFSTOUFS(mp);
    ip = VTOI(vp);
//    bflags = buf_flags(bp);
//
//	if (bp->b_iocmd == BIO_WRITE) {
//		if ((buf_flags(bp) & B_VALIDSUSPWRT) == 0 && vp != NULL && vnode_mount(vp) != NULL && (->mnt_kern_flag & MNTK_SUSPENDED) != 0)
//			panic("ffs_geom_strategy: bad I/O");
//		nocopy = buf_flags(bp) & B_NOCOPY;
//		buf_flags(bp) &= ~(B_VALIDSUSPWRT | B_NOCOPY);
//		if ((vp->v_vflag & VV_COPYONWRITE) && nocopy == 0 &&
//		    vp->v_rdev->si_snapdata != NULL) {
//			if ((buf_flags(bp) & B_CLUSTER) != 0) {
//				runningbufwakeup(bp);
//				TAILQ_FOREACH(tbp, &bp->b_cluster.cluster_head, b_cluster.cluster_entry) {
//					error = ffs_copyonwrite(vp, tbp);
//					if (error != 0 &&
//					    error != EOPNOTSUPP) {
//						bp->b_error = error;
//						bp->b_ioflags |= BIO_ERROR;
//						buf_flags(bp) &= ~B_BARRIER;
//						buf_biodone(bp);
//						return;
//					}
//				}
//				bp->b_runningbufspace = buf_size(bp);
//				OSAddAtomic64(&runningbufspace,
//					       bp->b_runningbufspace);
//			} else {
//				error = ffs_copyonwrite(vp, bp);
//				if (error != 0 && error != EOPNOTSUPP) {
//					bp->b_error = error;
//					bp->b_ioflags |= BIO_ERROR;
//					buf_flags(bp) &= ~B_BARRIER;
//					buf_biodone(bp);
//					return;
//				}
//			}
//		}
//#ifdef SOFTUPDATES
//		if ((buf_flags(bp) & B_CLUSTER) != 0) {
//			TAILQ_FOREACH(tbp, &bp->b_cluster.cluster_head,
//				      b_cluster.cluster_entry) {
//				if (!LIST_EMPTY(&tbp->b_dep))
//					buf_start(tbp);
//			}
//		} else {
//			if (!LIST_EMPTY(&bp->b_dep))
//				buf_start(bp);
//		}
//
//#endif
//		/*
//		 * Check for metadata that needs check-hashes and update them.
//		 */
//		switch (buf_xflags(bp) & BX_FSPRIV) {
//		case BX_CYLGRP:
//			((struct cg *)buf_dataptr(bp))->cg_ckhash = 0;
//			((struct cg *)buf_dataptr(bp))->cg_ckhash =
//			    calculate_crc32c(~0L, buf_dataptr(bp), buf_count(bp));
//			break;
//
//		case BX_SUPERBLOCK:
//		case BX_INODE:
//		case BX_INDIR:
//		case BX_DIR:
//			log_debug("Check-hash write is unimplemented!!!\n");
//			break;
//
//		case 0:
//			break;
//
//		default:
//			log_debug("multiple buffer types 0x%b\n",
//			    (u_int)(buf_xflags(bp) & BX_FSPRIV),
//			    PRINT_UFS_BUF_XFLAGS);
//			break;
//		}
//	}
//	if (bflags != BIO_READ && ffs_enxio_enable)
//		buf_setxflags(bp, BX_CVTENXIO);
	buf_strategy(ump->um_devvp, bp);
}

int
ffs_own_suspend(const struct mount *mp)
{
	return (1);
}

int ffs_sysctl(int *name, u_int namelen, user_addr_t oldp, size_t *oldlenp, user_addr_t newp,
               size_t newlen, vfs_context_t context)
{
    return ENOTSUP;
}

int ffs_getattrfs(struct mount *mp, struct vfs_attr *attrs, vfs_context_t context)
{
    struct vfsstatfs *stat = NULL;
    struct ufsmount *ump;
    struct fs *fs;

    ump = VFSTOUFS(mp);
    fs = ump->um_fs;
    stat = vfs_statfs(mp);
    
    if (fs->fs_magic != FS_UFS1_MAGIC && fs->fs_magic != FS_UFS2_MAGIC)
        panic("ffs_statfs");
    
    VFSATTR_RETURN(attrs, f_bsize, fs->fs_fsize);
    VFSATTR_RETURN(attrs, f_iosize, fs->fs_bsize);
    VFSATTR_RETURN(attrs, f_blocks, fs->fs_dsize);
    
    UFS_LOCK(ump);
    VFSATTR_RETURN(attrs, f_bfree, fs->fs_cstotal.cs_nbfree * fs->fs_frag +
                   fs->fs_cstotal.cs_nffree + dbtofsb(fs, fs->fs_pendingblocks));
    VFSATTR_RETURN(attrs, f_bavail, freespace(fs, fs->fs_minfree) + dbtofsb(fs, fs->fs_pendingblocks));
    VFSATTR_RETURN(attrs, f_files, fs->fs_ncg * fs->fs_ipg - UFS_ROOTINO);
    VFSATTR_RETURN(attrs, f_ffree, fs->fs_cstotal.cs_nifree + fs->fs_pendinginodes);
    UFS_UNLOCK(ump);
    
    if (VFSATTR_IS_ACTIVE(attrs, f_signature)){
        if (fs->fs_magic == FS_UFS1_MAGIC) {
            VFSATTR_RETURN(attrs, f_signature, ((uint16_t) FS_UFS1_MAGIC));
        } else {
            VFSATTR_RETURN(attrs, f_signature, ((uint16_t) FS_UFS2_MAGIC));
        };
    }
    
    if (VFSATTR_IS_ACTIVE(attrs, f_objcount))
        VFSATTR_RETURN(attrs, f_objcount, (uint64_t)(stat->f_files - stat->f_ffree));
    if (VFSATTR_IS_ACTIVE(attrs, f_filecount))
        VFSATTR_RETURN(attrs, f_filecount, (uint64_t)(stat->f_files - stat->f_ffree));
//    if (VFSATTR_IS_ACTIVE(attrs, f_dircount)); // unsupported. we could count through the cg's but too expensive.
    if (VFSATTR_IS_ACTIVE(attrs, f_maxobjcount))
        VFSATTR_RETURN(attrs, f_maxobjcount, (uint64_t)stat->f_files);

    if (VFSATTR_IS_ACTIVE(attrs, f_capabilities)) {
        int extracaps = 0, nfs_export = 0;
        long nfsflags = FREEBSD_MNT_EXRDONLY | FREEBSD_MNT_DEFEXPORTED | FREEBSD_MNT_EXPORTANON |
                        FREEBSD_MNT_EXKERB | FREEBSD_MNT_EXPUBLIC | FREEBSD_MNT_EXTLS |
                        FREEBSD_MNT_EXTLSCERT | FREEBSD_MNT_EXTLSCERTUSER;
        if ((fs->fs_flags & FS_GJOURNAL) != 0){
            extracaps = VOL_CAP_FMT_JOURNAL;
            if (vfs_flags(mp) & MNT_JOURNALED)
                extracaps |= VOL_CAP_FMT_JOURNAL_ACTIVE;
        }
        
        
        if ((ump->um_compat->mnt_flag & nfsflags) != 0){
            nfs_export |= VOL_CAP_INT_NFSEXPORT;
        }

        /* Capabilities we support */
        attrs->f_capabilities.capabilities[VOL_CAPABILITIES_FORMAT] =
        VOL_CAP_FMT_SYMBOLICLINKS|
        VOL_CAP_FMT_HARDLINKS|
        VOL_CAP_FMT_SPARSE_FILES|
        VOL_CAP_FMT_CASE_SENSITIVE|
        VOL_CAP_FMT_CASE_PRESERVING|
        VOL_CAP_FMT_FAST_STATFS|
        VOL_CAP_FMT_2TB_FILESIZE|
        extracaps;
        attrs->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] =
        
        VOL_CAP_INT_VOL_RENAME|
        VOL_CAP_INT_FLOCK;
        attrs->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED1] = 0;
        attrs->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED2] = 0;

        /* Capabilities we know about */
        attrs->f_capabilities.valid[VOL_CAPABILITIES_FORMAT] =
        VOL_CAP_FMT_PERSISTENTOBJECTIDS |
        VOL_CAP_FMT_SYMBOLICLINKS |
        VOL_CAP_FMT_HARDLINKS |
        VOL_CAP_FMT_JOURNAL |
        VOL_CAP_FMT_JOURNAL_ACTIVE |
        VOL_CAP_FMT_NO_ROOT_TIMES |
        VOL_CAP_FMT_SPARSE_FILES |
        VOL_CAP_FMT_ZERO_RUNS |
        VOL_CAP_FMT_CASE_SENSITIVE |
        VOL_CAP_FMT_CASE_PRESERVING |
        VOL_CAP_FMT_FAST_STATFS|
        VOL_CAP_FMT_2TB_FILESIZE;
        attrs->f_capabilities.valid[VOL_CAPABILITIES_INTERFACES] =
        VOL_CAP_INT_SEARCHFS |
        VOL_CAP_INT_ATTRLIST |
        nfs_export |
        VOL_CAP_INT_READDIRATTR |
        VOL_CAP_INT_EXCHANGEDATA |
        VOL_CAP_INT_COPYFILE |
        VOL_CAP_INT_ALLOCATE |
        VOL_CAP_INT_VOL_RENAME |
        VOL_CAP_INT_ADVLOCK |
        VOL_CAP_INT_FLOCK ;
        attrs->f_capabilities.valid[VOL_CAPABILITIES_RESERVED1] = 0;
        attrs->f_capabilities.valid[VOL_CAPABILITIES_RESERVED2] = 0;
        VFSATTR_SET_SUPPORTED(attrs, f_capabilities);
    }
    
    if (VFSATTR_IS_ACTIVE(attrs, f_attributes)) {
        attrs->f_attributes.validattr.commonattr = UFS_ATTR_CMN_NATIVE;
        attrs->f_attributes.validattr.volattr = UFS_ATTR_VOL_SUPPORTED;
        attrs->f_attributes.validattr.dirattr = UFS_ATTR_DIR_SUPPORTED;
        attrs->f_attributes.validattr.fileattr = UFS_ATTR_FILE_SUPPORTED;
        attrs->f_attributes.validattr.forkattr = UFS_ATTR_FORK_SUPPORTED;

        attrs->f_attributes.nativeattr.commonattr = UFS_ATTR_CMN_NATIVE;
        attrs->f_attributes.nativeattr.volattr = UFS_ATTR_VOL_NATIVE;
        attrs->f_attributes.nativeattr.dirattr = UFS_ATTR_DIR_NATIVE;
        attrs->f_attributes.nativeattr.fileattr = UFS_ATTR_FILE_NATIVE;
        attrs->f_attributes.nativeattr.forkattr = UFS_ATTR_FORK_NATIVE;
        VFSATTR_SET_SUPPORTED(attrs, f_attributes);
    }
    
    if (VFSATTR_IS_ACTIVE(attrs, f_vol_name)) {
        size_t len = strlen((char*)fs->fs_volname);
        (void)strncpy(attrs->f_vol_name, (char*)fs->fs_volname, len);
        attrs->f_vol_name[len] = 0;
        VFSATTR_SET_SUPPORTED(attrs, f_vol_name);
    }
    
    if (VFSATTR_IS_ACTIVE(attrs, f_create_time)) {
        attrs->f_create_time.tv_sec = fs->fs_mtime; // just give out mtime.
        attrs->f_create_time.tv_nsec = 0;
        VFSATTR_SET_SUPPORTED(attrs, f_create_time);
    }
    
    return 0;
}

static int verify_volumelabel(u_char *volumelabel)
{
    int i = -1;
    while (isalnum(volumelabel[++i]) || volumelabel[i] == '_' || volumelabel[i] == '-');
    if (volumelabel[i] != '\0') {
        log_debug("bad volume label. Valid characters " "are alphanumerics, dashes, and underscores.");
        return (1);
    }
    if (strlen((char*)volumelabel) >= MAXVOLLEN) {
        log_debug("bad volume label. Length is longer than %d.", MAXVOLLEN);
        return (1);
    }
    return (0);
}

int
ffs_setattrfs(mount_t mp, struct vfs_attr *attrs, vfs_context_t context)
{
    int error = 0;
    if (VFSATTR_IS_ACTIVE(attrs, f_vol_name)) {
        struct fs *fsp = VFSTOUFS(mp)->um_fs;
        const char *name = attrs->f_vol_name;
        size_t name_length = strlen(name);
      
        if (name_length > MAXVOLLEN) {
            log_debug("EXT2-fs: %s: Warning volume label too long, truncating.\n", __FUNCTION__);
            name_length = MAXVOLLEN;
        }
      
        error = verify_volumelabel((u_char*)name);
        if (name_length && !error) {
            bzero(&fsp->fs_volname[0], MAXVOLLEN);
            bcopy (name, &fsp->fs_volname[0], name_length);
            fsp->fs_fmod = 1;
            VFSATTR_SET_SUPPORTED(attrs, f_vol_name);
        } else
            error = EINVAL;
    }
    
    trace_return (error);
}

#ifdef	DDB
#ifdef SOFTUPDATES

/* defined in ffs_softdep.c */
extern void db_print_ffs(struct ufsmount *ump);

DB_SHOW_COMMAND(ffs, db_show_ffs)
{
	struct mount *mp;
	struct ufsmount *ump;

	if (have_addr) {
		ump = VFSTOUFS((struct mount *)addr);
		db_print_ffs(ump);
		return;
	}

	TAILQ_FOREACH(mp, &mountlist, mnt_list) {
		if (!strcmp(vfs_statfs(mp)->f_fstypename, ufs_vfsconf.vfc_name))
			db_print_ffs(VFSTOUFS(mp));
	}
}

#endif	/* SOFTUPDATES */
#endif	/* DDB */


int ffs_vget_snapdir(struct mount *mp, struct vnode **vpp, struct vfs_context *context)
{
    int error;
    struct vnode *rootvp, *snapvp;
    struct componentname cnp = {0};
    struct vfs_vget_args vargs = {0};
        
    error = ufs_root(mp, &rootvp, context); // get rootdir
    if (error){
        trace_return (error);
    }
    
    cnp.cn_nameiop = LOOKUP;
    cnp.cn_pnbuf = "/"UFS_SNAPDIR;
    cnp.cn_nameptr = cnp.cn_pnbuf+1;
    cnp.cn_namelen = (int)strlen(UFS_SNAPDIR);
    
    vargs.cnp = &cnp;
    vargs.dvp = rootvp;
    error = VFS_VGET(mp, UFS_SNAPDIR_INO, &vargs, &snapvp, context);
    
    if (error == 0 && vpp){
        *vpp = snapvp;
    } else {
        vnode_put(rootvp);
    }
    
    trace_return (error);
}

