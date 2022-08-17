/*-
 * SPDX-License-Identifier: (BSD-2-Clause-FreeBSD AND BSD-3-Clause)
 *
 * Copyright (c) 2002, 2003 Networks Associates Technology, Inc.
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Marshall
 * Kirk McKusick and Network Associates Laboratories, the Security
 * Research Division of Network Associates, Inc. under DARPA/SPAWAR
 * contract N66001-01-C-8035 ("CBOSS"), as part of the DARPA CHATS
 * research program
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (c) 1982, 1986, 1989, 1993
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
 *	from: @(#)ufs_readwrite.c	8.11 (Berkeley) 5/8/95
 * from: $FreeBSD: .../ufs/ufs_readwrite.c,v 1.96 2002/08/12 09:22:11 phk ...
 *	@(#)ffs_vnops.c	8.15 (Berkeley) 5/14/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/xattr.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/conf.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/ubc.h>

#include <freebsd/compat/compat.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>

#include <ufs/ufsX_vnops.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

#define    EXTATTR_NAMESPACE_EMPTY          0x00000000
#define    EXTATTR_NAMESPACE_EMPTY_STRING    "empty"
#define    EXTATTR_NAMESPACE_USER           0x00000001
#define    EXTATTR_NAMESPACE_USER_STRING     "user"
#define    EXTATTR_NAMESPACE_SYSTEM         0x00000002
#define    EXTATTR_NAMESPACE_SYSTEM_STRING   "system"

#define	ALIGNED_TO(ptr, s)	\
	(((uintptr_t)(ptr) & (_Alignof(s) - 1)) == 0)

#ifdef DIRECTIO
extern int	ffs_rawread(struct vnode *vp, struct uio *uio, int *workdone);
#endif
int ffs_extread(struct vnode *vp, struct uio *uio, int ioflag);
int ffs_extwrite(struct vnode *vp, struct uio *uio, int ioflag, struct vfs_context *context);

/*
 * Synch an open file.
 */
/* ARGSUSED */
int
ffs_fsync(struct vnop_fsync_args *ap)
{
	struct vnode *vp;
	int error;

	vp = ap->a_vp;
retry:
	error = ffs_syncvnode(vp, ap->a_waitfor, 0);
	if (error)
		return (error);
	if (ap->a_waitfor == MNT_WAIT && DOINGSOFTDEP(vp)) {
		error = softdep_fsync(vp);
		if (error)
			return (error);

		/*
		 * The softdep_fsync() function may drop vp lock,
		 * allowing for dirty buffers to reappear on the
		 * bo_dirty list. Recheck and resync as needed.
		 */
		if ((vnode_vtype(vp) == VREG || vnode_vtype(vp) == VDIR) &&
		    ubc_getcred(vp) != NOCRED) {
			goto retry;
		}
	}
	if (ffs_fsfail_cleanup(VFSTOUFS(vnode_mount(vp)), 0))
		return (ENXIO);
	return (0);
}

struct sync_iterate_args {
    bool abort;
    bool still_dirty;
    int unlocked;
    int waitfor;
    int flags;
    int passes;
    int wait;
    ufs_lbn_t lbn;
    int error;
};

static int
ffs_syncvnode_callback(buf_t bp, void *arg)
{
    struct sync_iterate_args *ap = (struct sync_iterate_args *)arg;
    vnode_t vp = buf_vnode(bp);
    struct inode *ip = VTOI(vp);
    struct ufsmount *ump = ITOUMP(ip);
    struct bufpriv *bpriv = buf_fsprivate(bp);
    
    /* if buffer dependencies don't exist, skip the buffer ... TODO: maybe relse it? */
    if (bpriv == NULL)
        return BUF_CLAIMED;
    
    /*
     * Reasons to skip this buffer: it has already been considered
     * on this pass, the buffer has dependencies that will cause
     * it to be redirtied and it has not already been deferred,
     * or it is already being written.
     */
    if ((buf_vflags(bp) & BV_SCANNED) != 0){
        return BUF_CLAIMED; // continue
    }
    
    buf_setvflags(bp, BV_SCANNED);
    
    /*
     * Flush indirects in order, if requested.
     *
     * Note that if only datasync is requested, we can
     * skip indirect blocks when softupdates are not
     * active. Otherwise we must flush them with data,
     * since dependencies prevent data block writes.
     */
    
    if (ap->waitfor == MNT_WAIT && buf_lblkno(bp) <= -UFS_NDADDR &&
        (lbn_level(buf_lblkno(bp)) >= ap->passes || ((ap->flags & DATA_ONLY) != 0 && !DOINGSOFTDEP(vp))))
        return BUF_CLAIMED;
    
    if (buf_lblkno(bp) > ap->lbn)
        panic("ffs_syncvnode: syncing truncated data.");

    if ((buf_flags(bp) & B_DELWRI) == 0)
        panic("ffs_fsync: not dirty");
    /*
     * Check for dependencies and potentially complete them.
     */
    if (bpriv != NULL && !LIST_EMPTY(&bpriv->b_dep) && (ap->error = softdep_sync_buf(vp, bp,
                                    ap->wait ? MNT_WAIT : MNT_NOWAIT)) != 0) {
        /*
         * Lock order conflict, buffer was already unlocked,
         * and vnode possibly unlocked.
         */
        if (ap->error == ERECYCLE) {
            if (vnode_isrecycled(vp)){
                ap->error = (EBADF);
                return BUF_CLAIMED_DONE;
            }
            ap->unlocked = true;
            if (DOINGSOFTDEP(vp) && ap->waitfor == MNT_WAIT && (ap->error = softdep_sync_metadata(vp)) != 0) {
                if (ffs_fsfail_cleanup(ump, ap->error))
                    ap->error = 0;
                ap->abort = true;
                ap->error = (ap->unlocked && ap->error == 0) ? ERECYCLE : ap->error;
                return BUF_CLAIMED_DONE;
                
            }
            /* Re-evaluate inode size */
            ap->lbn = lblkno(ITOFS(ip), (ip->i_size + ITOFS(ip)->fs_bsize - 1));
            goto next;
        }
        /* I/O error. */
        if (ap->error != EBUSY) {
            ap->abort = true;
            return (BUF_CLAIMED_DONE);
        }
    }
    if (ap->wait) {;
        ap->error = bwrite(bp);
        if (ffs_fsfail_cleanup(ump, ap->error))
            ap->error = 0;
        if (ap->error != 0){
            ap->abort = true;
            return BUF_CLAIMED_DONE;
        }
    } else {
        (void) buf_bawrite(bp);
    }
next:
    return BUF_RETURNED;
}

// checks if there are still any dirty indirect-block buffers in the vnode.
static int
ffs_syncvnode_scan_databufs(buf_t bp, void *arg)
{
    bool *still_dirty = (bool*)arg;
    
    if (buf_lblkno(bp) > -UFS_NDADDR) {
        *still_dirty = true;
        return BUF_CLAIMED;
    }

    return BUF_CLAIMED_DONE;
}

int
ffs_syncvnode(struct vnode *vp, int waitfor, int flags)
{
	struct inode *ip;
	struct ufsmount *ump;
	ufs_lbn_t lbn;
	bool still_dirty, unlocked;
    struct sync_iterate_args ap;

	ip = VTOI(vp);
	ip->i_flag &= ~IN_NEEDSYNC;
	ump = VFSTOUFS(vnode_mount(vp));
    

	/*
	 * When doing MNT_WAIT we must first flush all dependencies
	 * on the inode.
	 */
	if (DOINGSOFTDEP(vp) && waitfor == MNT_WAIT && (ap.error = softdep_sync_metadata(vp)) != 0) {
		if (ffs_fsfail_cleanup(ump, ap.error))
            ap.error = 0;
		return (ap.error);
	}

    ap.abort = 0;
    ap.flags = flags;
    ap.waitfor = waitfor;
    
    /*
     * Flush all dirty buffers associated with a vnode.
     */
    ap.passes = 0;
    ap.wait = false; /* Always do an async pass first. */
    ap.lbn = lbn = lblkno(ITOFS(ip), (ip->i_size + ITOFS(ip)->fs_bsize - 1));
    
loop:
    ap.error = 0;
    
    // flags... we want buf_iterate to notify us of locked dirty bufs.
    buf_iterate(vp, ffs_syncvnode_callback, BUF_SKIP_LOCKED | BUF_SCAN_DIRTY, &ap);
    
    if (ap.abort){
        return ap.error;
    }
    
    unlocked = false;
    
	if (waitfor != MNT_WAIT) {
		if ((flags & NO_INO_UPDT) != 0)
			return (unlocked ? ERECYCLE : 0);
        ap.error = ffs_update(vp, 0);
		if (ap.error == 0 && unlocked)
			ap.error = ERECYCLE;
		return (ap.error);
	}
	/* Drain IO to see if we're done. */
    vnode_waitforwrites(vp, 0, 0, 0, "ffs_syncvnode");
	/*
	 * Block devices associated with filesystems may have new I/O
	 * requests posted for them even if the vnode is locked, so no
	 * amount of trying will get them clean.  We make several passes
	 * as a best effort.
	 *
	 * Regular files may need multiple passes to flush all dependency
	 * work as it is possible that we must write once per indirect
	 * level, once for the leaf, and once for the inode and each of
	 * these will be done with one sync and one async pass.
     **
     * We won't really do data-syncs on macOS because fdatasync(2) doesn't exist.
     * HFS+ has data-sync like functionality,
     * but it's designed differently (I think, I haven't studied it thoroughly)
	 */
	if (vnode_hasdirtyblks(vp)) {
		if ((flags & DATA_ONLY) == 0) {
            still_dirty = true;
		} else {
			/*
			 * For data-only sync, dirty indirect buffers
			 * are ignored.
			 */
            still_dirty = false;
            buf_iterate(vp, ffs_syncvnode_scan_databufs, BUF_SKIP_LOCKED | BUF_SCAN_DIRTY, &still_dirty);
		}

        if (still_dirty) {
			/* Write the inode after sync passes to flush deps. */
            if (ap.wait && DOINGSOFTDEP(vp) && (flags & NO_INO_UPDT) == 0) {
				ffs_update(vp, 1);
			}
			/* switch between sync/async. */
            ap.wait = !ap.wait;
            if (ap.wait || ++ap.passes < UFS_NIADDR + 2)
				goto loop;
		}
	}
    
    ap.error = 0;
	if ((flags & DATA_ONLY) == 0) {
		if ((flags & NO_INO_UPDT) == 0)
            ap.error = ffs_update(vp, 1);
		if (DOINGSUJ(vp))
			softdep_journal_fsync(VTOI(vp));
	} else if ((ip->i_flags & (IN_SIZEMOD | IN_IBLKDATA)) != 0) {
        ap.error = ffs_update(vp, 1);
	}
	if (ap.error == 0 && unlocked)
        ap.error = ERECYCLE;
	return (ap.error);
}

static int
ffs_read_hole(struct uio *uio, long xfersize, long *size)
{
	ssize_t saved_resid, tlen;
    char zero[xfersize];
	int error;

    memset(zero, 0x0, sizeof(xfersize));
    
	while (xfersize > 0) {
		tlen = xfersize;
		saved_resid = uio_resid(uio);
		error = uiomove(zero, (int)tlen, uio);
		if (error != 0)
			return (error);
		tlen = saved_resid - uio_resid(uio);
		xfersize -= tlen;
		*size -= tlen;
	}
	return (0);
}

/*
 * Vnode op for reading.
 */
int
ffs_read(struct vnop_read_args *ap)
/* {
	struct vnode *a_vp;
	struct uio *a_uio;
	int a_ioflag;
	struct ucred *a_cred;
} */
{
	struct vnode *vp;
	struct inode *ip;
	struct uio *uio;
	struct fs *fs;
	struct buf *bp;
	ufs_lbn_t lbn, nextlbn;
	off_t bytesinfile;
	long size, xfersize, blkoffset;
	ssize_t orig_resid;
	int error, ioflag;

	vp = ap->a_vp;
	uio = ap->a_uio;
	ioflag = ap->a_ioflag;
	if (ap->a_ioflag & FREEBSD_IO_EXT)
#ifdef notyet
		return (ffs_extread(vp, uio, ioflag));
#else
		panic("ffs_read+FREEBSD_IO_EXT");
#endif
#ifdef DIRECTIO
	if ((ioflag & IO_DIRECT) != 0) {
		int workdone;

		error = ffs_rawread(vp, uio, &workdone);
		if (error != 0 || workdone != 0)
			return error;
	}
#endif

	ip = VTOI(vp);

#ifdef INVARIANTS
	if (uio_rw(uio) != UIO_READ)
		panic("ffs_read: mode");

	if (vnode_vtype(vp) == VLNK) {
		if ((int)ip->i_size < vfs_maxsymlen(vnode_mount(vp)))
			panic("ffs_read: short symlink");
	} else if (vnode_vtype(vp) != VREG && vnode_vtype(vp) != VDIR)
		panic("ffs_read: type %d",  vnode_vtype(vp));
#endif
	orig_resid = uio_resid(uio);
	ASSERT(orig_resid >= 0, ("ffs_read: uio_resid(uio) < 0"));
	if (orig_resid == 0)
		return (0);
	ASSERT(uio_offset(uio) >= 0, ("ffs_read: uio_offset(uio) < 0"));
	fs = ITOFS(ip);
	if (uio_offset(uio) < ip->i_size &&
	    uio_offset(uio) >= fs->fs_maxfilesize)
		return (EOVERFLOW);

    if (vnode_vtype(vp) == VREG) {
        bp = NULL;
        
        /*
         * UBC always exists for regular files
         */
        error = cluster_read(vp, uio, orig_resid, ioflag);
    } else for (error = 0, bp = NULL; uio_resid(uio) > 0; bp = NULL) {
		if ((bytesinfile = ip->i_size - uio_offset(uio)) <= 0)
			break;
		lbn = lblkno(fs, uio_offset(uio));
		nextlbn = lbn + 1;

		/*
		 * size of buffer.  The buffer representing the
		 * end of the file is rounded up to the size of
		 * the block type ( fragment or full block,
		 * depending ).
		 */
		size = blksize(fs, ip, lbn);
		blkoffset = blkoff(fs, uio_offset(uio));

		/*
		 * The amount we want to transfer in this iteration is
		 * one FS block less the amount of the data before
		 * our startpoint (duh!)
		 */
		xfersize = fs->fs_bsize - blkoffset;

		/*
		 * But if we actually want less than the block,
		 * or the file doesn't have a whole block more of data,
		 * then use the lesser number.
		 */
		if (uio_resid(uio) < xfersize)
			xfersize = uio_resid(uio);
		if (bytesinfile < xfersize)
			xfersize = bytesinfile;

        if (lblktosize(fs, nextlbn) >= ip->i_size) {
			/*
			 * Don't do readahead if this is the end of the file.
			 */
			error = bread(vp, (int)lbn, (int)size, NOCRED, 0, &bp);
		} else {
			/*
			 * Failing all of the above, just read what the
			 * user asked for. Interestingly, the same as
			 * the first option above.
			 */
			error = bread(vp, (int)lbn, (int)size, NOCRED, 0, &bp);
		}
		if (error == EJUSTRETURN) {
			error = ffs_read_hole(uio, xfersize, &size);
			if (error == 0)
				continue;
		}
		if (error != 0) {
			buf_brelse(bp);
			bp = NULL;
			break;
		}

		/*
		 * We should only get non-zero b_resid when an I/O error
		 * has occurred, which should cause us to break above.
		 * However, if the short read did not cause an error,
		 * then we want to ensure that we do not uiomove bad
		 * or uninitialized data.
		 */
        
		size -= buf_resid(bp);
		if (size < xfersize) {
			if (size == 0)
				break;
			xfersize = size;
		}

        error = uiomove((char *)buf_dataptr(bp) +
                        blkoffset, (int)xfersize, uio);
		if (error)
			break;

		buf_brelse(bp);
	}

	/*
	 * This can only happen in the case of an error
	 * because the loop above resets bp to NULL on each iteration
	 * and on normal completion has not set a new value into it.
	 * so it must have come from a 'break' statement
	 */
	if (bp != NULL)
		buf_brelse(bp);

	if ((error == 0 || uio_resid(uio) != orig_resid) &&
	    (vfs_flags(vnode_mount(vp)) & (MNT_NOATIME | MNT_RDONLY)) == 0)
		UFS_INODE_SET_FLAG_SHARED(ip, IN_ACCESS);
	return (error);
}

/*
 * Vnode op for writing.
 */
int
ffs_write(struct vnop_write_args *ap)
/* {
	struct vnode *a_vp;
	struct uio *a_uio;
	int a_ioflag;
	struct ucred *a_cred;
} */
{
	struct vnode *vp;
	struct uio *uio;
	struct inode *ip;
	struct fs *fs;
	struct buf *bp;
	ufs_lbn_t lbn;
	off_t osize;
	ssize_t resid, ubc_resid, filepos;
    struct ucred *cred;
    struct vfs_context *context;
	int blkoffset, error, flags, ioflag, size, xfersize;

    flags = 0;
    context = ap->a_context;
    cred = vfs_context_ucred(context);
	vp = ap->a_vp;
	uio = ap->a_uio;
	ioflag = ap->a_ioflag;
	if (ap->a_ioflag & FREEBSD_IO_EXT)
#ifdef notyet
		return (ffs_extwrite(vp, uio, ioflag, ap->a_cred));
#else
		panic("ffs_write+FREEBSD_IO_EXT");
#endif

	ip = VTOI(vp);

#ifdef INVARIANTS
	if (uio_rw(uio) != UIO_WRITE)
		panic("ffs_write: mode");
#endif

	switch (vnode_vtype(vp)) {
	case VREG:
		if (ioflag & IO_APPEND)
			uio_setoffset(uio, ip->i_size);
		if ((ip->i_flags & APPEND) && uio_offset(uio) != ip->i_size)
			return (EPERM);
		/* FALLTHROUGH */
	case VLNK:
		break;
	case VDIR:
		panic("ffs_write: dir write");
		break;
	default:
		panic("ffs_write: type %p %d (%d,%d)", vp, (int)vnode_vtype(vp),
			(int)uio_offset(uio),
			(int)uio_resid(uio)
		);
	}

	ASSERT(uio_resid(uio) >= 0, ("ffs_write: uio_resid(uio) < 0"));
	ASSERT(uio_offset(uio) >= 0, ("ffs_write: uio_offset(uio) < 0"));
	fs = ITOFS(ip);
	if ((off_t)uio_offset(uio) + uio_resid(uio) > fs->fs_maxfilesize)
		return (EFBIG);

	resid = ubc_resid = uio_resid(uio);
    filepos = uio_offset(uio);
	osize = ip->i_size;
    xfersize = fs->fs_bsize;

    if (ioflag & IO_SYNC)
		flags = IO_SYNC;
	flags |= BA_UNMAPPED;

    if (vnode_vtype(vp) == VREG){
        /* handle write for ubc_info */
        int head_offset = 0;
        int tail_offset = 0;
        
        for (;ubc_resid > 0;){
            blkoffset = (int)blkoff(fs, filepos);
            xfersize = fs->fs_bsize - blkoffset;
            if (ubc_resid < xfersize)
                xfersize = (int)ubc_resid;
            if (filepos + xfersize > ip->i_size)
                size = (int)filepos + xfersize;
            filepos += xfersize;
            resid -= xfersize;
        }
        
        // if write offset starts within a block past the EOF
        if (blkoff(fs, uio_offset(uio)) < fs->fs_bsize && xfersize >= filepos) {
            flags = ioflag & ~(IO_TAILZEROFILL | IO_HEADZEROFILL | IO_NOZEROVALID | IO_NOZERODIRTY);
            /*
             * The first page is beyond current EOF (io_append), so as an
             * optimisation, we can pass IO_HEADZEROFILL.
             */
            flags |= IO_HEADZEROFILL;
            head_offset = (int)blkoff(fs, uio_offset(uio));
        }
        
        if (xfersize < fs->fs_bsize) {
            // the there's trailing free space on the last block at end of write, so zero it out
            flags |= IO_TAILZEROFILL;
            tail_offset = (int)filepos;
        }
        
        ubc_setsize(vp, filepos);
        error = cluster_write(vp, uio, osize, filepos, head_offset, tail_offset, flags);
    } else for (error = 0; uio_resid(uio) > 0;) {
		lbn = lblkno(fs, uio_offset(uio));
		blkoffset = (int)blkoff(fs, uio_offset(uio));
		xfersize = fs->fs_bsize - blkoffset;
		if (uio_resid(uio) < xfersize)
			xfersize = (int)uio_resid(uio);
		if (uio_offset(uio) + xfersize > ip->i_size)
			ubc_setsize(vp, uio_offset(uio) + xfersize);

		/*
		 * We must perform a read-before-write if the transfer size
		 * does not cover the entire buffer.
		 */
		if (fs->fs_bsize > xfersize)
			flags |= BA_CLRBUF;
		else
			flags &= ~BA_CLRBUF;
/* XXX is uio_offset(uio) the right thing here? */
		error = UFS_BALLOC(vp, uio_offset(uio), xfersize, context, flags, &bp);
		if (error != 0) {
			ubc_setsize(vp, ip->i_size);
			break;
		}
		if ((ioflag & (IO_SYNC | IO_NOCACHE)) == (IO_SYNC | IO_NOCACHE))
			buf_setflags(bp, B_NOCACHE);

		if (uio_offset(uio) + xfersize > ip->i_size) {
			ip->i_size = uio_offset(uio) + xfersize;
			DIP_SET(ip, i_size, ip->i_size);
			UFS_INODE_SET_FLAG(ip, IN_SIZEMOD | IN_CHANGE);
		}

		size = blksize(fs, ip, lbn) - buf_resid(bp);
		if (size < xfersize)
			xfersize = size;

        error = uiomove((char *)buf_dataptr(bp) + blkoffset, (int)xfersize, uio);

		/*
		 * If the buffer is not already filled and we encounter an
		 * error while trying to fill it, we have to clear out any
		 * garbage data from the pages instantiated for the buffer.
		 * If we do not, a failed uiomove() during a write can leave
		 * the prior contents of the pages exposed to a userland mmap.
		 *
		 * Note that we need only clear buffers with a transfer size
		 * equal to the block size because buffers with a shorter
		 * transfer size were cleared above by the call to UFS_BALLOC()
		 * with the BA_CLRBUF flag set.
		 *
		 * If the source region for uiomove identically mmaps the
		 * buffer, uiomove() performed the NOP copy, and the buffer
		 * content remains valid because the page fault handler
		 * validated the pages.
		 */
		if (error != 0 && (buf_flags(bp) & B_NOCACHE) == 1 &&
		    fs->fs_bsize == xfersize)
			buf_clear(bp);

		buf_setflags(bp, ioflag);

		/*
		 * If IO_SYNC each buffer is written synchronously.  Otherwise
		 * if we have a severe page deficiency write the buffer
		 * asynchronously.  Otherwise try to cluster, and if that
		 * doesn't do it then either do an async write (if O_DIRECT),
		 * or a delayed write (if not).
		 */
		if (ioflag & IO_SYNC) {
			(void)bwrite(bp);
		} else if ((ioflag & IO_SYNC) == 0) {
			buf_setflags(bp, B_CLUSTEROK);
			buf_bawrite(bp);
		} else if (xfersize + blkoffset == fs->fs_bsize) {
            buf_bawrite(bp);
        } else {
			buf_setflags(bp, B_CLUSTEROK);
			buf_bdwrite(bp);
		}
		if (error || xfersize == 0)
			break;
		UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
	}
	/*
	 * If we successfully wrote any data, and we are not the superuser
	 * we clear the setuid and setgid bits as a precaution against
	 * tampering.
	 */
	if ((ip->i_mode & (ISUID | ISGID)) && resid > uio_resid(uio) && cred) {
        if(kauth_cred_issuser(cred)){
			UFS_INODE_SET_MODE(ip, ip->i_mode & ~(ISUID | ISGID));
			DIP_SET(ip, i_mode, ip->i_mode);
		}
	}
	if (error) {
		if (ioflag & IO_UNIT) {
			(void)ffs_truncate(vp, osize, FREEBSD_IO_NORMAL | (ioflag & IO_SYNC), context);
            uio_setoffset(uio, uio_offset(uio) - resid - uio_resid(uio));
			uio_setresid(uio, resid);
		}
	} else if (resid > uio_resid(uio) && (ioflag & IO_SYNC)) {
		error = ffs_update(vp, 1);
		if (ffs_fsfail_cleanup(VFSTOUFS(vnode_mount(vp)), error))
			error = ENXIO;
	}
	return (error);
}

/*
 * Extended attribute area reading.
 */
int
ffs_extread(struct vnode *vp, struct uio *uio, int ioflag)
{
	struct inode *ip;
	struct ufs2_dinode *dp;
	struct fs *fs;
	struct buf *bp;
	ufs_lbn_t lbn, nextlbn;
	off_t bytesinfile;
	long size, xfersize, blkoffset;
	ssize_t orig_resid;
	int error;

	ip = VTOI(vp);
	fs = ITOFS(ip);
	dp = ip->i_din2;

#ifdef INVARIANTS
	if (uio_rw(uio) != UIO_READ || fs->fs_magic != FS_UFS2_MAGIC)
		panic("ffs_extread: mode");

#endif
	orig_resid = uio_resid(uio);
	ASSERT(orig_resid >= 0, ("ffs_extread: uio_resid(uio) < 0"));
	if (orig_resid == 0)
		return (0);
	ASSERT(uio_offset(uio) >= 0, ("ffs_extread: uio_offset(uio) < 0"));

	for (error = 0, bp = NULL; uio_resid(uio) > 0; bp = NULL) {
		if ((bytesinfile = dp->di_extsize - uio_offset(uio)) <= 0)
			break;
		lbn = lblkno(fs, uio_offset(uio));
		nextlbn = lbn + 1;

		/*
		 * size of buffer.  The buffer representing the
		 * end of the file is rounded up to the size of
		 * the block type ( fragment or full block,
		 * depending ).
		 */
		size = sblksize(fs, dp->di_extsize, lbn);
		blkoffset = blkoff(fs, uio_offset(uio));

		/*
		 * The amount we want to transfer in this iteration is
		 * one FS block less the amount of the data before
		 * our startpoint (duh!)
		 */
		xfersize = fs->fs_bsize - blkoffset;

		/*
		 * But if we actually want less than the block,
		 * or the file doesn't have a whole block more of data,
		 * then use the lesser number.
		 */
		if (uio_resid(uio) < xfersize)
			xfersize = uio_resid(uio);
		if (bytesinfile < xfersize)
			xfersize = bytesinfile;

		if (lblktosize(fs, nextlbn) >= dp->di_extsize) {
			/*
			 * Don't do readahead if this is the end of the info.
			 */
			error = buf_bread(vp, -1 - lbn, (int)size, NOCRED, &bp);
		} else {
			/*
			 * If we have a second block, then
			 * fire off a request for a readahead
			 * as well as a read. Note that the 4th and 5th
			 * arguments point to arrays of the size specified in
			 * the 6th argument.
			 */
			u_int nextsize = sblksize(fs, dp->di_extsize, nextlbn);

			nextlbn = -1 - nextlbn;
			error = buf_breadn(vp, -1 - lbn,
                        (int)size, &nextlbn, (int*)&nextsize, 1, NOCRED, &bp);
		}
		if (error) {
			buf_brelse(bp);
			bp = NULL;
			break;
		}

		/*
		 * We should only get non-zero b_resid when an I/O error
		 * has occurred, which should cause us to break above.
		 * However, if the short read did not cause an error,
		 * then we want to ensure that we do not uiomove bad
		 * or uninitialized data.
		 */
		size -= buf_resid(bp);
		if (size < xfersize) {
			if (size == 0)
				break;
			xfersize = size;
		}

		error = uiomove((char *)buf_dataptr(bp) + blkoffset,
					(int)xfersize, uio);
		if (error)
			break;
		buf_brelse(bp);
	}

	/*
	 * This can only happen in the case of an error
	 * because the loop above resets bp to NULL on each iteration
	 * and on normal completion has not set a new value into it.
	 * so it must have come from a 'break' statement
	 */
	if (bp != NULL)
		buf_brelse(bp);
	return (error);
}

/*
 * Extended attribute area writing.
 */
int
ffs_extwrite(struct vnode *vp, struct uio *uio, int ioflag, struct vfs_context *context)
{
	struct inode *ip;
	struct ufs2_dinode *dp;
	struct fs *fs;
	struct buf *bp;
	ufs_lbn_t lbn;
	off_t osize;
	ssize_t resid;
    kauth_cred_t cred;
	int blkoffset, error, flags, size, xfersize;

	ip = VTOI(vp);
	fs = ITOFS(ip);
	dp = ip->i_din2;
    cred = vfs_context_ucred(context);

#ifdef INVARIANTS
	if (uio_rw(uio) != UIO_WRITE || fs->fs_magic != FS_UFS2_MAGIC)
		panic("ffs_extwrite: mode");
#endif

	if (ioflag & IO_APPEND)
		uio_setoffset(uio, dp->di_extsize);
	ASSERT(uio_offset(uio) >= 0, ("ffs_extwrite: uio_offset(uio) < 0"));
	ASSERT(uio_resid(uio) >= 0, ("ffs_extwrite: uio_resid(uio) < 0"));
	if ((off_t)uio_offset(uio) + uio_resid(uio) > UFS_NXADDR * fs->fs_bsize)
		return (EFBIG);

	resid = uio_resid(uio);
	osize = dp->di_extsize;
	flags = FREEBSD_IO_EXT;
	if (ioflag & IO_SYNC)
		flags |= IO_SYNC;

	for (error = 0; uio_resid(uio) > 0;) {
		lbn = lblkno(fs, uio_offset(uio));
		blkoffset = (int)blkoff(fs, uio_offset(uio));
		xfersize = fs->fs_bsize - blkoffset;
		if (uio_resid(uio) < xfersize)
			xfersize = (int)uio_resid(uio);

		/*
		 * We must perform a read-before-write if the transfer size
		 * does not cover the entire buffer.
		 */
		if (fs->fs_bsize > xfersize)
			flags |= BA_CLRBUF;
		else
			flags &= ~BA_CLRBUF;
		error = UFS_BALLOC(vp, uio_offset(uio), xfersize,
		    context, flags, &bp);
		if (error != 0)
			break;
		/*
		 * If the buffer is not valid we have to clear out any
		 * garbage data from the pages instantiated for the buffer.
		 * If we do not, a failed uiomove() during a write can leave
		 * the prior contents of the pages exposed to a userland
		 * mmap().  XXX deal with uiomove() errors a better way.
		 */
		if ((buf_flags(bp) & B_NOCACHE) == 1 && fs->fs_bsize <= xfersize)
			buf_clear(bp);

		if (uio_offset(uio) + xfersize > dp->di_extsize) {
			dp->di_extsize = (int) uio_offset(uio) + xfersize;
			UFS_INODE_SET_FLAG(ip, IN_SIZEMOD | IN_CHANGE);
		}

		size = sblksize(fs, dp->di_extsize, lbn) - buf_resid(bp);
		if (size < xfersize)
			xfersize = size;

		error =
		    uiomove((char *)buf_dataptr(bp) + blkoffset, (int)xfersize, uio);

		buf_setflags(bp, ioflag);

		/*
		 * If IO_SYNC each buffer is written synchronously.  Otherwise
		 * if we have a severe page deficiency write the buffer
		 * asynchronously.  Otherwise try to cluster, and if that
		 * doesn't do it then either do an async write (if O_DIRECT),
		 * or a delayed write (if not).
		 */
		if (ioflag & IO_SYNC) {
			(void)bwrite(bp);
		} else if (xfersize + blkoffset == fs->fs_bsize)
			buf_bawrite(bp);
		else
			buf_bdwrite(bp);
		if (error || xfersize == 0)
			break;
		UFS_INODE_SET_FLAG(ip, IN_CHANGE);
	}
	/*
	 * If we successfully wrote any data, and we are not the superuser
	 * we clear the setuid and setgid bits as a precaution against
	 * tampering.
	 */
	if ((ip->i_mode & (ISUID | ISGID)) && resid > uio_resid(uio)) {
		if (kauth_cred_issuser(cred)) {
			UFS_INODE_SET_MODE(ip, ip->i_mode & ~(ISUID | ISGID));
			dp->di_mode = ip->i_mode;
		}
	}
	if (error) {
		if (ioflag & IO_UNIT) {
			(void)ffs_truncate(vp, osize, FREEBSD_IO_EXT | (ioflag&IO_SYNC), context);
            uio_setoffset(uio, uio_offset(uio) - (resid - uio_resid(uio)));
			uio_setresid(uio, resid);
		}
	} else if (resid > uio_resid(uio) && (ioflag & IO_SYNC))
		error = ffs_update(vp, 1);
	return (error);
}

/*
 * Vnode operating to retrieve a named extended attribute.
 *
 * Locate a particular EA (nspace:name) in the area (ptr:length), and return
 * the length of the EA, and possibly the pointer to the entry and to the data.
 */
static int
ffs_findextattr(u_char *ptr, u_int length, int nspace, const char *name, struct extattr **eapp, u_char **eac)
{
	struct extattr *eap, *eaend;
	size_t nlen;

	nlen = strlen(name);
	ASSERT(ALIGNED_TO(ptr, struct extattr), ("unaligned"));
	eap = (struct extattr *)ptr;
	eaend = (struct extattr *)(ptr + length);
	for (; eap < eaend; eap = EXTATTR_NEXT(eap)) {
		ASSERT(EXTATTR_NEXT(eap) <= eaend,
		    ("extattr next %p beyond %p", EXTATTR_NEXT(eap), eaend));
		if (eap->ea_namespace != nspace || eap->ea_namelength != nlen
		    || memcmp(eap->ea_name, name, nlen) != 0)
			continue;
		if (eapp != NULL)
			*eapp = eap;
		if (eac != NULL)
			*eac = EXTATTR_CONTENT(eap);
		return (EXTATTR_CONTENT_SIZE(eap));
	}
	return (-1);
}

static int
ffs_rdextattr(u_char **p, struct vnode *vp, struct vfs_context *context)
{
	const struct extattr *eap, *eaend, *eapnext;
	struct inode *ip;
	struct ufs2_dinode *dp;
	struct fs *fs;
	struct uio *luio;
	struct iovec liovec;
	u_int easize;
	int error;
	u_char *eae;

	ip = VTOI(vp);
	fs = ITOFS(ip);
	dp = ip->i_din2;
	easize = dp->di_extsize;
	if ((off_t)easize > UFS_NXADDR * fs->fs_bsize)
		return (EFBIG);

	eae = malloc(easize, 0, M_WAITOK);

	liovec.iov_base = eae;
	liovec.iov_len = easize;

    luio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
    uio_addiov(luio, (user_addr_t)&liovec, easize);

	error = ffs_extread(vp, luio, FREEBSD_IO_EXT | IO_SYNC);
	if (error) {
        free(eae, M_UFSMNT);
		return (error);
	}
	/* Validate disk xattrfile contents. */
	for (eap = (void *)eae, eaend = (void *)(eae + easize); eap < eaend;
	    eap = eapnext) {
		eapnext = EXTATTR_NEXT(eap);
		/* Bogusly short entry or bogusly long entry. */
		if (eap->ea_length < sizeof(*eap) || eapnext > eaend) {
			free(eae, M_UFSMNT);
			return (ESTALE);
		}
	}
	*p = eae;
	return (0);
}

static void
ffs_lock_ea(struct vnode *vp)
{
	struct inode *ip;

	ip = VTOI(vp);
	ixlock(ip);
	while (ip->i_flag & IN_EA_LOCKED) {
		UFS_INODE_SET_FLAG(ip, IN_EA_LOCKWAIT);
		msleep(&ip->i_ea_refs, NULL, PINOD + 2, "ufs_ea", 0);
	}
	UFS_INODE_SET_FLAG(ip, IN_EA_LOCKED);
	iunlock(ip);
}

static void
ffs_unlock_ea(struct vnode *vp)
{
	struct inode *ip;

	ip = VTOI(vp);
	ixlock(ip);
	if (ip->i_flag & IN_EA_LOCKWAIT)
		wakeup(&ip->i_ea_refs);
	ip->i_flag &= ~(IN_EA_LOCKED | IN_EA_LOCKWAIT);
	iunlock(ip);
}

static int
ffs_open_ea(struct vnode *vp, struct vfs_context *context)
{
	struct inode *ip;
	struct ufs2_dinode *dp;
	int error;

	ip = VTOI(vp);

	ffs_lock_ea(vp);
	if (ip->i_ea_area != NULL) {
		ip->i_ea_refs++;
		ffs_unlock_ea(vp);
		return (0);
	}
	dp = ip->i_din2;
	error = ffs_rdextattr(&ip->i_ea_area, vp, context);
	if (error) {
		ffs_unlock_ea(vp);
		return (error);
	}
	ip->i_ea_len = dp->di_extsize;
	ip->i_ea_error = 0;
	ip->i_ea_refs++;
	ffs_unlock_ea(vp);
	return (0);
}

/*
 * Vnode extattr transaction commit/abort
 */
static int
ffs_close_ea(struct vnode *vp, int commit, struct vfs_context *context)
{
	struct inode *ip;
	struct uio *luio;
	struct iovec liovec;
	int error;
	struct ufs2_dinode *dp;

	ip = VTOI(vp);

	ffs_lock_ea(vp);
	if (ip->i_ea_area == NULL) {
		ffs_unlock_ea(vp);
		return (EINVAL);
	}
	dp = ip->i_din2;
	error = ip->i_ea_error;
	if (commit && error == 0) {
		ASSERT_VNOP_ELOCKED(vp, "ffs_close_ea commit");

		liovec.iov_base = ip->i_ea_area;
		liovec.iov_len = ip->i_ea_len;
        
        luio = uio_create(1, 0, UIO_SYSSPACE, UIO_WRITE);
        uio_addiov(luio, (user_addr_t)&liovec, ip->i_ea_len);

		/* XXX: I'm not happy about truncating to zero size */
		if (ip->i_ea_len < dp->di_extsize)
			error = ffs_truncate(vp, 0, FREEBSD_IO_EXT, context);
		error = ffs_extwrite(vp, luio, FREEBSD_IO_EXT | IO_SYNC, context);
	}
	if (--ip->i_ea_refs == 0) {
		free(ip->i_ea_area, M_UFSMNT);
		ip->i_ea_area = NULL;
		ip->i_ea_len = 0;
		ip->i_ea_error = 0;
	}
	ffs_unlock_ea(vp);
	return (error);
}

/*
 * Vnode extattr strategy routine for fifos.
 *
 * We need to check for a read or write of the external attributes.
 * Otherwise we just fall through and do the usual thing.
 */
int
ffsext_strategy(struct vnop_strategy_args *ap)
/*
struct vnop_strategy_args {
	struct vnodeop_desc *a_desc;
	struct vnode *a_vp;
	struct buf *a_bp;
};
*/
{
	struct vnode *vp;
    struct buf *bp;
	daddr64_t lbn;

	bp = ap->a_bp;
    vp = buf_vnode(bp);
	lbn = buf_lblkno(bp);
    
    if ((I_IS_UFS2(VTOI(vp)) && lbn < 0 && lbn >= -UFS_NXADDR) ||
        (vnode_vtype(vp) == VFIFO)){
        return VNOP_STRATEGY(bp);
    }
	panic("spec nodes went here");
    __builtin_unreachable();
}

/*
 * Vnode extattr transaction commit/abort
 **
 * Vnode extattr transaction commit/abort
 */

/*
 * Vnode operation to remove a named attribute.
 */
int
ffs_deleteextattr(struct vnop_removexattr_args *ap)
/*
vnop_deleteextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	IN const char *a_name;
	IN struct ucred *a_cred;
	IN struct vfs_context *a_td;
};
*/
{
	struct inode *ip;
	struct extattr *eap;
	uint32_t ul;
    long i;
	int olen, error, easize, tmplen;
	u_char *eae;
	void *tmp;

	ip = VTOI(ap->a_vp);

	if (vnode_vtype(ap->a_vp) == VCHR || vnode_vtype(ap->a_vp) == VBLK)
		return (EOPNOTSUPP);

	if (strlen(ap->a_name) == 0)
		return (EINVAL);

	if (vfs_flags(vnode_mount(ap->a_vp)) & MNT_RDONLY)
		return (EROFS);

//	error = extattr_check_cred(ap->a_vp, ap->a_name, ap->a_cred, ap->a_td, VWRITE);
//	if (error) {
//		/*
//		 * ffs_lock_ea is not needed there, because the vnode
//		 * must be exclusively locked.
//		 */
//		if (ip->i_ea_area != NULL && ip->i_ea_error == 0)
//			ip->i_ea_error = error;
//		return (error);
//	}

	error = ffs_open_ea(ap->a_vp, ap->a_context);
	if (error)
		return (error);

	/* CEM: delete could be done in-place instead */
	eae = malloc(ip->i_ea_len, 0, M_WAITOK);
	bcopy(ip->i_ea_area, eae, ip->i_ea_len);
	easize = ip->i_ea_len;

	olen = ffs_findextattr(eae, easize, 0, ap->a_name, &eap, NULL);
	if (olen == -1) {
		/* delete but nonexistent */
        free(eae, M_UFSMNT);
		ffs_close_ea(ap->a_vp, 0, ap->a_context);
		return (ENOATTR);
	}
	ul = eap->ea_length;
	i = (u_char *)EXTATTR_NEXT(eap) - eae;
	bcopy(EXTATTR_NEXT(eap), eap, easize - i);
	easize -= ul;

	tmp = ip->i_ea_area;
    tmplen = ip->i_ea_len;
	ip->i_ea_area = eae;
	ip->i_ea_len = easize;
    free(tmp, M_UFSMNT);
	error = ffs_close_ea(ap->a_vp, 1, ap->a_context);
	return (error);
}

/*
 * Vnode operation to retrieve a named extended attribute.
 */
int
ffs_getextattr(struct vnop_getxattr_args *ap)
/*
vnop_getextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	IN const char *a_name;
	INOUT struct uio *a_uio;
	OUT size_t *a_size;
	IN struct ucred *a_cred;
	IN struct vfs_context *a_td;
};
*/
{
	struct inode *ip;
	u_char *eae, *p;
	unsigned easize;
	int error, ealen;

	ip = VTOI(ap->a_vp);

	if (vnode_vtype(ap->a_vp) == VCHR || vnode_vtype(ap->a_vp) == VBLK)
		return (EOPNOTSUPP);

//	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace, ap->a_cred, ap->a_td, VREAD);
//	if (error)
//		return (error);

	error = ffs_open_ea(ap->a_vp, ap->a_context);
	if (error)
		return (error);

	eae = ip->i_ea_area;
	easize = ip->i_ea_len;

	ealen = ffs_findextattr(eae, easize, 0, ap->a_name, NULL, &p);
	if (ealen >= 0) {
		error = 0;
		if (ap->a_size != NULL)
			*ap->a_size = ealen;
		else if (ap->a_uio != NULL)
			error = uiomove((const char*)p, ealen, ap->a_uio);
	} else
		error = ENOATTR;

	ffs_close_ea(ap->a_vp, 0, ap->a_context);
	return (error);
}

/*
 * Vnode operation to retrieve extended attributes on a vnode.
 */
int
ffs_listextattr(struct vnop_listxattr_args *ap)
/*
vnop_listextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	INOUT struct uio *a_uio;
	OUT size_t *a_size;
	IN struct ucred *a_cred;
	IN struct vfs_context *a_td;
};
*/
{
	struct inode *ip;
	struct extattr *eap, *eaend;
	int error, ealen;

	ip = VTOI(ap->a_vp);
    
    if(I_IS_UFS1(ip)){
        return (ENOTSUP);
    }

	if (vnode_vtype(ap->a_vp) == VCHR || vnode_vtype(ap->a_vp) == VBLK)
		return (EOPNOTSUPP);

//	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace, ap->a_cred, ap->a_td, VREAD);
//	if (error)
//		return (error);

	error = ffs_open_ea(ap->a_vp, ap->a_context);
	if (error)
		return (error);

	error = 0;
	if (ap->a_size != NULL)
		*ap->a_size = 0;

	ASSERT(ALIGNED_TO(ip->i_ea_area, struct extattr), ("unaligned"));
	eap = (struct extattr *)ip->i_ea_area;
	eaend = (struct extattr *)(ip->i_ea_area + ip->i_ea_len);
	for (; error == 0 && eap < eaend; eap = EXTATTR_NEXT(eap)) {
		ASSERT(EXTATTR_NEXT(eap) <= eaend, ("extattr next %p beyond %p", EXTATTR_NEXT(eap), eaend));
		if (eap->ea_namespace != 0)
			continue;

		ealen = eap->ea_namelength;
		if (ap->a_size != NULL)
			*ap->a_size += ealen + 1;
		else if (ap->a_uio != NULL)
			error = uiomove((const char*)&eap->ea_namelength, ealen + 1,
			    ap->a_uio);
	}

	ffs_close_ea(ap->a_vp, 0, ap->a_context);
	return (error);
}

/*
 * Vnode operation to set a named attribute.
 */
int
ffs_setextattr(struct vnop_setxattr_args *ap)
/*
vnop_setextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	IN const char *a_name;
	INOUT struct uio *a_uio;
	IN struct ucred *a_cred;
	IN struct vfs_context *a_td;
};
*/
{
	struct inode *ip;
	struct fs *fs;
	struct extattr *eap;
	uint32_t ealength, ul;
	ssize_t ealen;
    long i;
	int olen, eapad1, eapad2, error, easize, tmplen;
	u_char *eae;
	void *tmp;

	ip = VTOI(ap->a_vp);
	fs = ITOFS(ip);

	if (vnode_vtype(ap->a_vp) == VCHR || vnode_vtype(ap->a_vp) == VBLK)
		return (EOPNOTSUPP);

	if (strlen(ap->a_name) == 0)
		return (EINVAL);

	/* XXX Now unsupported API to delete EAs using NULL uio. */
	if (ap->a_uio == NULL)
		return (EOPNOTSUPP);

	if (vfs_flags(vnode_mount(ap->a_vp)) & MNT_RDONLY)
		return (EROFS);

	ealen = uio_resid(ap->a_uio);
	if (ealen < 0 || ealen > lblktosize(fs, UFS_NXADDR))
		return (EINVAL);

//	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace, ap->a_cred, ap->a_td, VWRITE);
//	if (error) {
//		/*
//		 * ffs_lock_ea is not needed there, because the vnode
//		 * must be exclusively locked.
//		 */
//		if (ip->i_ea_area != NULL && ip->i_ea_error == 0)
//			ip->i_ea_error = error;
//		return (error);
//	}

	error = ffs_open_ea(ap->a_vp, ap->a_context);
	if (error)
		return (error);

	ealength = sizeof(uint32_t) + 3 + (int)strlen(ap->a_name);
	eapad1 = roundup2(ealength, 8) - ealength;
	eapad2 = roundup2(ealen, 8) - (int)ealen;
	ealength += eapad1 + ealen + eapad2;

	/*
	 * CEM: rewrites of the same size or smaller could be done in-place
	 * instead.  (We don't acquire any fine-grained locks in here either,
	 * so we could also do bigger writes in-place.)
	 */
	eae = malloc(ip->i_ea_len + ealength, 0, M_WAITOK);
	bcopy(ip->i_ea_area, eae, ip->i_ea_len);
	easize = ip->i_ea_len;

	olen = ffs_findextattr(eae, easize, 0, ap->a_name, &eap, NULL);
        if (olen == -1) {
		/* new, append at end */
		ASSERT(ALIGNED_TO(eae + easize, struct extattr),
		    ("unaligned"));
		eap = (struct extattr *)(eae + easize);
		easize += ealength;
	} else {
		ul = eap->ea_length;
		i = (u_char *)EXTATTR_NEXT(eap) - eae;
		if (ul != ealength) {
			bcopy(EXTATTR_NEXT(eap), (u_char *)eap + ealength,
			    easize - i);
			easize += (ealength - ul);
		}
	}
	if (easize > lblktosize(fs, UFS_NXADDR)) {
        free(eae, M_UFSMNT);
		ffs_close_ea(ap->a_vp, 0, ap->a_context);
		if (ip->i_ea_area != NULL && ip->i_ea_error == 0)
			ip->i_ea_error = ENOSPC;
		return (ENOSPC);
	}
	eap->ea_length = ealength;
    eap->ea_namespace = EXTATTR_NAMESPACE_SYSTEM; // ap->a_attrnamespace;
	eap->ea_contentpadlen = eapad2;
	eap->ea_namelength = strlen(ap->a_name);
	memcpy(eap->ea_name, ap->a_name, strlen(ap->a_name));
	bzero(&eap->ea_name[strlen(ap->a_name)], eapad1);
	error = uiomove(EXTATTR_CONTENT(eap), (int)ealen, ap->a_uio);
	if (error) {
        free(eae, M_UFSMNT);
		ffs_close_ea(ap->a_vp, 0, ap->a_context);
		if (ip->i_ea_area != NULL && ip->i_ea_error == 0)
			ip->i_ea_error = error;
		return (error);
	}
	bzero((u_char *)EXTATTR_CONTENT(eap) + ealen, eapad2);

	tmp = ip->i_ea_area;
    tmplen = ip->i_ea_len;
	ip->i_ea_area = eae;
	ip->i_ea_len = easize;
    free(tmp, M_UFSMNT);
	error = ffs_close_ea(ap->a_vp, 1, ap->a_context);
	return (error);
}

SYSCTL_DECL(_vfs_ffs);
static int use_buf_pager = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, use_buf_pager, CTLFLAG_RW, &use_buf_pager, 0, "Always use buffer pager instead of bmap");

static daddr_t __unused
ffs_gbp_getblkno(struct vnode *vp, vm_offset_t off)
{
	return ((daddr_t)lblkno(VFSTOUFS(vnode_mount(vp))->um_fs, off));
}

static int __unused
ffs_gbp_getblksz(struct vnode *vp, daddr_t lbn)
{

	return (blksize(VFSTOUFS(vnode_mount(vp))->um_fs, VTOI(vp), lbn));
}

#if 0
static int
ffs_getpages(struct vnop_getpages_args *ap)
{
	struct vnode *vp;
	struct ufsmount *um;

	vp = ap->a_vp;
	um = VFSTOUFS(vnode_mount(vp));

	if (!use_buf_pager && um->um_devvp->v_bufobj.bo_bsize <= PAGE_SIZE)
		return (vnode_pager_generic_getpages(vp, ap->a_m, ap->a_count, ap->a_rbehind, ap->a_rahead, NULL, NULL));
	return (vfs_bio_getpages(vp, ap->a_m, ap->a_count, ap->a_rbehind, ap->a_rahead, ffs_gbp_getblkno, ffs_gbp_getblksz));
}

static int
ffs_getpages_async(struct vnop_getpages_async_args *ap)
{
	struct vnode *vp;
	struct ufsmount *um;
	bool do_iodone;
	int error;

	vp = ap->a_vp;
	um = VFSTOUFS(vnode_mount(vp));
	do_iodone = true;

	if (um->um_devvp->v_bufobj.bo_bsize <= PAGE_SIZE) {
		error = vnode_pager_generic_getpages(vp, ap->a_m, ap->a_count,
		    ap->a_rbehind, ap->a_rahead, ap->a_iodone, ap->a_arg);
		if (error == 0)
			do_iodone = false;
	} else {
		error = vfs_bio_getpages(vp, ap->a_m, ap->a_count,
		    ap->a_rbehind, ap->a_rahead, ffs_gbp_getblkno,
		    ffs_gbp_getblksz);
	}
	if (do_iodone && ap->a_iodone != NULL)
		ap->a_iodone(ap->a_arg, ap->a_m, ap->a_count, error);

	return (error);
}
#endif

/*
 * call paths. only call for VREG vnodes.
 *
 * buf_strategy() -> cluster_bp() -> cluster_bp_ext() -> ubc_blktooff()
 * buf_bwrite() -> brecover_data() -> ubc_blktooff()
 * buf_brelse() -> ubc_blktooff()
 * buf_getblk() -> ubc_blktooff() ... note, only for VREG files
 * Blktooff converts a logical block number to a file offset
 */
int
ffs_blktooff(struct vnop_blktooff_args *ap)
/* {
    struct vnode *a_vp;
    daddr64_t a_lblkno;
    off_t *a_offset;
} */
{
    struct fs *fs;

    if (ap->a_vp == NULL)
        return (EINVAL);

    fs = ITOFS(VTOI(ap->a_vp));

    *ap->a_offset = (off_t)lblktosize(fs, ap->a_lblkno);

    return (0);
}

/* Blktooff converts a logical block number to a file offset */
int
ffs_offtoblk(struct vnop_offtoblk_args *ap)
/* {
    struct vnode *a_vp;
    off_t a_offset;
    daddr64_t *a_lblkno;
} */
{
    register struct fs *fs;

    if (ap->a_vp == NULL)
        return (EINVAL);

    fs = ITOFS(VTOI(ap->a_vp));

    *ap->a_lblkno = (daddr64_t)lblkno(fs, ap->a_offset);

    return (0);
}
