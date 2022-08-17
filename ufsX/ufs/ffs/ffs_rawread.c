/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2000-2003 Tor Egge
 * All rights reserved.
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/ubc.h>


#include <freebsd/compat/compat.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

#include <sys/kernel.h>
#include <sys/sysctl.h>

static int ffs_rawread_readahead(struct vnode *vp,
				 caddr_t udata,
				 off_t offset,
				 size_t len,
				 vfs_context_t context,
				 struct buf *bp);
static int ffs_rawread_main(struct vnode *vp,
			    struct uio *uio);

static int ffs_rawread_sync(struct vnode *vp);

int ffs_rawread(struct vnode *vp, struct uio *uio, int *workdone);

SYSCTL_DECL(_vfs_ffs);

static uint32_t ffsraw_pbuf_zone;

static int allowrawread = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, allowrawread, CTLFLAG_RW, &allowrawread, 0,
	   "Flag to enable raw reads");

static int rawreadahead = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, rawreadahead, CTLFLAG_RW, &rawreadahead, 0,
	   "Flag to enable readahead for long raw reads");

static void
ffs_rawread_setup(void *arg __unused)
{

	ffsraw_pbuf_zone = pbuf_zsecond_create("ffsrawpbuf",
	    (nswbuf > 100 ) ?  (nswbuf - (nswbuf >> 4)) : nswbuf - 8);
}
SYSINIT(ffs_raw, SI_SUB_VM_CONF, SI_ORDER_ANY, ffs_rawread_setup, NULL);

static int
ffs_rawread_sync(struct vnode *vp)
{
	int error;
	int upgraded;
	struct bufobj *bo;
	struct mount *mp;

	/* Check for dirty mmap, pending writes and dirty buffers */
	ixlock(ip);
	if (vnode_isreg(vp) && ubc_getcred(vp) != NOCRED) {
		iunlock(ip);
		
		if (vn_start_write(vp, &mp, V_NOWAIT) != 0) {
			if (VOP_ISLOCKED(vp) != UFS_LOCK_EXCLUSIVE)
				upgraded = 1;
			else
				upgraded = 0;
			VNOP_UNLOCK(vp);
			(void) vn_start_write(vp, &mp, V_WAIT);
			VNOP_LOCK(vp, UFS_LOCK_EXCLUSIVE);
		} else if (VOP_ISLOCKED(vp) != UFS_LOCK_EXCLUSIVE) {
			upgraded = 1;
			/* Upgrade to exclusive lock, this might block */
			VNOP_LOCK(vp, UFS_LOCK_UPGRADE);
		} else
			upgraded = 0;
			
		
		ixlock(ip);
		/* Check if vnode was reclaimed while unlocked. */
		if (VN_IS_DOOMED(vp)) {
			iunlock(ip);
			if (upgraded != 0)
				VNOP_LOCK(vp, UFS_LOCK_DOWNGRADE);
			vn_finished_write(mp);
			return (EIO);
		}
		/* Wait for pending writes to complete */
        iunlock(ip);
        error = vnode_waitforwrites(vp, 0, 0, 0, "ffs_rawread_sync");
		if (error != 0) {
			/* XXX: can't happen with a zero timeout ??? */
			BO_UNLOCK(bo);
			if (upgraded != 0)
				VNOP_LOCK(vp, UFS_LOCK_DOWNGRADE);
			vn_finished_write(mp);
			return (error);
		}
		/* Flush dirty buffers */
        if ((error = ffs_syncvnode(vp, MNT_WAIT, 0)) != 0) {
            if (upgraded != 0)
                VNOP_LOCK(vp, UFS_LOCK_DOWNGRADE);
            vn_finished_write(mp);
            return (error);
        }
        
        
		if (upgraded != 0)
			VNOP_LOCK(vp, UFS_LOCK_DOWNGRADE);
		vn_finished_write(mp);
	} else {
		iunlock(ip);
	}
	return 0;
}

static int
ffs_rawread_readahead(struct vnode *vp,
		      caddr_t udata,
		      off_t offset,
		      size_t len,
		      vfs_context_t context,
		      struct buf *bp)
{
	int error;
	u_int iolen;
	off_t blockno;
	int blockoff;
	int bsize;
	struct vnode *dp;
	int bforwards;
	struct inode *ip;
	ufs2_daddr_t blkno;

	bsize = (int) vfs_statfs(vnode_mount(vp))->f_iosize;

	ip = VTOI(vp);
	dp = ITODEVVP(ip);

	iolen = ((vm_offset_t) udata) & PAGE_MASK;
	buf_setcount(bp, (int)len);
	if (buf_count(bp) + iolen > buf_size(bp)) {
		buf_setcount(bp, buf_size(bp));
		if (iolen != 0)
			buf_setcount(bp, buf_count(bp) - PAGE_SIZE);
	}

	blockno = offset / bsize;
	blockoff = (offset % bsize) / DEV_BSIZE;
	if ((daddr_t) blockno != blockno) {
		return EINVAL; /* blockno overflow */
	}

    buf_setlblkno(bp, blockno);
    buf_setlblkno(bp, blockno);

	error = ufs_bmaparray(vp, buf_lblkno(bp), &blkno, NULL, &bforwards, NULL);
	if (error != 0)
		return error;
	if (blkno == -1) {
		/* Fill holes with NULs to preserve semantics */
		if (buf_count(bp) + blockoff * DEV_BSIZE > bsize)
			buf_setcount(bp, bsize - blockoff * DEV_BSIZE);
        
        buf_map();
        
        (void)thread_block(THREAD_CONTINUE_NULL); // yield
        buf_clear(bp);
		/* Mark operation completed */
        buf_biodone(bp);
		return 0;
	}
	buf_setlblkno(bp, blkno + blockoff);
	buf_setoffset(bp, (blkno + blockoff) * DEV_BSIZE); // FIXME: fix this

	if (buf_count(bp) + blockoff * DEV_BSIZE > bsize * (1 + bforwards))
		buf_setcount(bp, bsize * (1 + bforwards) - blockoff * DEV_BSIZE);
    
    BO_STRATEGY(bp);
	return 0;
}

static int
ffs_rawread_main(struct vnode *vp,
		 struct uio *uio)
{
	int error, nerror;
	struct buf *bp, *nbp, *tbp;
	u_int iolen;
	caddr_t udata;
	long resid;
	off_t offset;
	vfs_context_t context;

	context = vfs_context_current();
	udata = (caddr_t)uio_curriovbase(uio);
	resid = uio_resid(uio);
	offset = uio_offset(uio);

	/*
	 * keep the process from being swapped
	 */

	error = 0;
	nerror = 0;

	bp = NULL;
	nbp = NULL;

	while (resid > 0) {
		
		if (bp == NULL) { /* Setup first read */
			bp = buf_geteblk(<#int size#>);
            buf_setvnode(bp, vp);
			error = ffs_rawread_readahead(vp, udata, offset, resid, context, bp);
			if (error != 0)
				break;
			
			if (resid > buf_size(bp)) { /* Setup fist readahead */
				if (rawreadahead != 0) 
					nbp = uma_zalloc(ffsraw_pbuf_zone,
					    M_NOWAIT);
				else
					nbp = NULL;
				if (nbp != NULL) {
					pbgetvp(vp, nbp); // this is buf_setvnode
					
					nerror = ffs_rawread_readahead(vp, 
								       udata +
								       buf_size(bp),
								       offset +
								       buf_size(bp),
								       resid -
								       buf_size(bp),
								       td,
								       nbp);
					if (nerror) {
						pbrelvp(nbp);
						uma_zfree(ffsraw_pbuf_zone,
						    nbp);
						nbp = NULL;
					}
				}
			}
		}
		
		bwait(bp, PRIBIO, "rawrd");
		vunmapbuf(bp);
		
		iolen = buf_count(bp) - buf_resid(bp);
		if (iolen == 0 && (bp->b_ioflags & BIO_ERROR) == 0) {
			nerror = 0;	/* Ignore possible beyond EOF error */
			break; /* EOF */
		}
		
		if ((bp->b_ioflags & BIO_ERROR) != 0) {
			error = bp->b_error;
			break;
		}
		resid -= iolen;
		udata += iolen;
		offset += iolen;
		if (iolen < buf_size(bp)) {
			/* Incomplete read.  Try to read remaining part */
			error = ffs_rawread_readahead(vp,
						      udata,
						      offset,
						      buf_size(bp) - iolen,
						      td,
						      bp);
			if (error != 0)
				break;
		} else if (nbp != NULL) { /* Complete read with readahead */
			
			tbp = bp;
			bp = nbp;
			nbp = tbp;
			
			if (resid <= buf_size(bp)) { /* No more readaheads */
				pbrelvp(nbp);
				uma_zfree(ffsraw_pbuf_zone, nbp);
				nbp = NULL;
			} else { /* Setup next readahead */
				nerror = ffs_rawread_readahead(vp,
							       udata +
							       buf_size(bp),
							       offset +
							       buf_size(bp),
							       resid -
							       buf_size(bp),
							       td,
							       nbp);
				if (nerror != 0) {
					pbrelvp(nbp);
					uma_zfree(ffsraw_pbuf_zone, nbp);
					nbp = NULL;
				}
			}
		} else if (nerror != 0) {/* Deferred Readahead error */
			break;		
		}  else if (resid > 0) { /* More to read, no readahead */
			error = ffs_rawread_readahead(vp, udata, offset,
						      resid, td, bp);
			if (error != 0)
				break;
		}
	}

	if (bp != NULL) {
		pbrelvp(bp);
		uma_zfree(ffsraw_pbuf_zone, bp);
	}
	if (nbp != NULL) {			/* Run down readahead buffer */
		bwait(nbp, PRIBIO, "rawrd");
		vunmapbuf(nbp);
		pbrelvp(nbp);
		uma_zfree(ffsraw_pbuf_zone, nbp);
	}

	if (error == 0)
		error = nerror;
	PRELE(td->td_proc);
	uio->uio_iov->iov_base = udata;
	uio_resid(uio) = resid;
	uio_setoffset(uio, offset);
	return error;
}

int
ffs_rawread(struct vnode *vp,
	    struct uio *uio,
	    int *workdone)
{
	if (allowrawread != 0 &&
	    uio->uio_iovcnt == 1 && 
	    uio->uio_segflg == UIO_USERSPACE &&
	    uio_resid(uio) == uio->uio_iov->iov_len &&
	    (((uio->uio_td != NULL) ? uio->uio_td : current_thread())->td_pflags &
	     TDP_DEADLKTREAT) == 0) {
		int secsize;		/* Media sector size */
		off_t filebytes;	/* Bytes left of file */
		int blockbytes;		/* Bytes left of file in full blocks */
		int partialbytes;	/* Bytes in last partial block */
		int skipbytes;		/* Bytes not to read in ffs_rawread */
		struct inode *ip;
		int error;
		

		/* Only handle sector aligned reads */
		ip = VTOI(vp);
		secsize = ITODEVVP(ip)->i_bsize;
		if ((uio_offset(uio) & (secsize - 1)) == 0 &&
		    (uio_resid(uio) & (secsize - 1)) == 0) {
			
			/* Sync dirty pages and buffers if needed */
			error = ffs_rawread_sync(vp);
			if (error != 0)
				return error;
			
			/* Check for end of file */
			if (ip->i_size > uio_offset(uio)) {
				filebytes = ip->i_size - uio_offset(uio);

				/* No special eof handling needed ? */
				if (uio_resid(uio) <= filebytes) {
					*workdone = 1;
					return ffs_rawread_main(vp, uio);
				}
				
				partialbytes = ((unsigned int) ip->i_size) %
				    ITOFS(ip)->fs_bsize;
				blockbytes = (int) filebytes - partialbytes;
				if (blockbytes > 0) {
					skipbytes = uio_resid(uio) -
						blockbytes;
					uio_resid(uio) = blockbytes;
					error = ffs_rawread_main(vp, uio);
					uio_resid(uio) += skipbytes;
					if (error != 0)
						return error;
					/* Read remaining part using buffer */
				}
			}
		}
	}
	*workdone = 0;
	return 0;
}
