/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1989, 1991, 1993
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
 *	@(#)ufs_bmap.c	8.7 (Berkeley) 3/21/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/fsctl.h>
#include <sys/stat.h>
#include <sys/buf.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>

#include <freebsd/sys/compat.h>

#include <ufs/ffs/fs.h>

static ufs_lbn_t lbn_count(struct ufsmount *, int);
static int readindir(struct vnode *, ufs_lbn_t, ufs2_daddr_t, struct buf **);

/*
 * Bmap converts the logical block number of a file to its physical block
 * number on the disk. The conversion is done by using the logical block
 * number to index into the array of block pointers described by the dinode.
 */
int
ufs_bmap(ap)
struct vnop_blockmap_args /* {
    struct vnodeop_desc *a_desc;
    vnode_t a_vp;
    off_t a_foffset;
    size_t a_size;
    daddr64_t *a_bpn;
    size_t *a_run;
    void *a_poff;
    int a_flags;
    vfs_context_t a_context;
} */ *ap;
{
	ufs2_daddr_t blkno, lbn;
    struct inode *ip;
    struct ufsmount *ump;
    daddr64_t retsize;
    int error, run, iosize;
    size_t size;

	/*
	 * Check for underlying vnode requests and ensure that logical
	 * to physical mapping is requested.
	 */
	if (ap->a_bpn == NULL)
		return (0);
    
    ip = VTOI(ap->a_vp);
    ump = ITOUMP(ip);
    iosize = (int)vfs_statfs(ump->um_mountp)->f_iosize;
    lbn = lblkno(ump->um_fs, (ufs2_daddr_t)ap->a_foffset);
    size = ap->a_size;
    
	error = ufs_bmaparray(ap->a_vp, lbn, &blkno, NULL, &run, NULL);
    if(error != 0)
        return(error);

    if (ap->a_bpn)
        *ap->a_bpn = blkno;
    
    if (ap->a_poff){
        *(int *)ap->a_poff = 0;
    }
    
    if (ap->a_run != NULL) {
        if (lbn < 0) {
            // indirect block
            retsize = (run + 1) * iosize;
        } else if (-1 == blkno || 0 == run) {
            // sparse hole or no contiguous blocks
            retsize = blksize(ump->um_fs, ip, lbn);
        } else {
            // 1 or more contiguous blocks
            retsize = run * iosize;
            retsize += blksize(ump->um_fs, ip, (lbn + run));
        }
        
        if (retsize < size)
            *ap->a_run = retsize;
        else
            *ap->a_run = size;
    }

    
	*ap->a_bpn = blkno;
	return (error);
}

static int
readindir(vp, lbn, daddr, bpp)
	struct vnode *vp;
	ufs_lbn_t lbn;
	ufs2_daddr_t daddr;
	struct buf **bpp;
{
	struct buf *bp;
	struct mount *mp;
	struct ufsmount *ump;
	int error;

	mp = vnode_mount(vp);
	ump = VFSTOUFS(mp);

	bp = buf_getblk(vp, lbn, (int)vfs_statfs(mp)->f_iosize, 0, 0, BLK_READ);
	if (buf_fromcache(bp) == 0) {
		KASSERT(daddr != 0, ("readindir: indirect block not in cache"));
        
        buf_setblkno(bp, (daddr64_t)blkptrtodb(ump, daddr));
        buf_setflags(bp, B_READ);
		buf_strategy(ump->um_devvp, bp);
        
		error = buf_biowait(bp);
		if (error != 0) {
			buf_brelse(bp);
			return (error);
		}
	}
	*bpp = bp;
	return (0);
}

/*
 * Indirect blocks are now on the vnode for the file.  They are given negative
 * logical block numbers.  Indirect blocks are addressed by the negative
 * address of the first data block to which they point.  Double indirect blocks
 * are addressed by one less than the address of the first indirect block to
 * which they point.  Triple indirect blocks are addressed by one less than
 * the address of the first double indirect block to which they point.
 *
 * ufs_bmaparray does the bmap conversion, and if requested returns the
 * array of logical blocks which must be traversed to get to a block.
 * Each entry contains the offset into that block that gets you to the
 * next block and the disk address of the block (if it is assigned).
 */

int
ufs_bmaparray(vp, bn, bnp, nbp, runp, runb)
	struct vnode *vp;
	ufs2_daddr_t bn;
	ufs2_daddr_t *bnp;
	struct buf *nbp;
	int *runp;
	int *runb;
{
	struct inode *ip;
	struct buf *bp;
	struct ufsmount *ump;
	struct mount *mp;
	struct indir a[UFS_NIADDR+1], *ap;
	ufs2_daddr_t daddr;
	ufs_lbn_t metalbn;
	int error, num, maxrun = 0;
    struct vfsioattr vfsio;
	int *nump;

	ap = NULL;
	ip = VTOI(vp);
	mp = vnode_mount(vp);
	ump = VFSTOUFS(mp);
    vfs_ioattr(mp, &vfsio);
 
	if (runp) {
        // TODO: this is for searching... mp->mnt_iosize_max
		maxrun = vfsio.io_maxwritecnt / vfs_statfs(mp)->f_iosize - 1;
		*runp = 0;
	}

	if (runb) {
		*runb = 0;
	}

	ap = a;
	nump = &num;
	error = ufs_getlbns(vp, bn, ap, nump);
	if (error)
		return (error);

	num = *nump;
	if (num == 0) {
		if (bn >= 0 && bn < UFS_NDADDR) {
			*bnp = blkptrtodb(ump, DIP(ip, i_db[bn]));
		} else if (bn < 0 && bn >= -UFS_NXADDR) {
			*bnp = blkptrtodb(ump, ip->i_din2->di_extb[-1 - bn]);
			if (*bnp == 0)
				*bnp = -1;
			if (nbp == NULL) {
				/* indirect block not found */
				return (EINVAL);
			}
			buf_setxflags(nbp, BX_ALTDATA);
			return (0);
		} else {
			/* blkno out of range */
			return (EINVAL);
		}
		/*
		 * Since this is FFS independent code, we are out of
		 * scope for the definitions of BLK_NOCOPY and
		 * BLK_SNAP, but we do know that they will fall in
		 * the range 1..um_seqinc, so we use that test and
		 * return a request for a zeroed out buffer if attempts
		 * are made to read a BLK_NOCOPY or BLK_SNAP block.
		 */
		if ((ip->i_flags & SF_SNAPSHOT) && DIP(ip, i_db[bn]) > 0 &&
		    DIP(ip, i_db[bn]) < ump->um_seqinc) {
			*bnp = -1;
		} else if (*bnp == 0) {
			if (ip->i_flags & SF_SNAPSHOT)
				*bnp = blkptrtodb(ump, bn * ump->um_seqinc);
			else
				*bnp = -1;
		} else if (runp) {
			ufs2_daddr_t bnb = bn;
			for (++bn; bn < UFS_NDADDR && *runp < maxrun &&
			    is_sequential(ump, DIP(ip, i_db[bn - 1]),
			    DIP(ip, i_db[bn]));
			    ++bn, ++*runp);
			bn = bnb;
			if (runb && (bn > 0)) {
				for (--bn; (bn >= 0) && (*runb < maxrun) &&
					is_sequential(ump, DIP(ip, i_db[bn]),
						DIP(ip, i_db[bn+1]));
						--bn, ++*runb);
			}
		}
		return (0);
	}

	/* Get disk address out of indirect block array */
	daddr = DIP(ip, i_ib[ap->in_off]);

	for (bp = NULL, ++ap; --num; ++ap) {
		/*
		 * Exit the loop if there is no disk address assigned yet and
		 * the indirect block isn't in the cache, or if we were
		 * looking for an indirect block and we've found it.
		 */

		metalbn = ap->in_lbn;
		if ((daddr == 0 && !incore(vp, metalbn)) || metalbn == bn)
			break;
        
		/*
		 * If we get here, we've either got the block in the cache
		 * or we have a disk address for it, go fetch it.
		 */
		if (bp)
			buf_brelse(bp);
		error = readindir(vp, metalbn, daddr, &bp);
		if (error != 0)
			return (error);

		if (I_IS_UFS1(ip))
			daddr = ((ufs1_daddr_t *)buf_dataptr(bp))[ap->in_off];
		else
			daddr = ((ufs2_daddr_t *)buf_dataptr(bp))[ap->in_off];
		if ((error = UFS_CHECK_BLKNO(mp, ip->i_number, (int)daddr,
		     (int)vfs_statfs(mp)->f_iosize)) != 0) {
			buf_qrelse(bp);
			return (error);
		}
		if (I_IS_UFS1(ip)) {
			if (num == 1 && daddr && runp) {
				for (bn = ap->in_off + 1;
				    bn < MNINDIR(ump) && *runp < maxrun &&
				    is_sequential(ump,
				    ((ufs1_daddr_t *)buf_dataptr(bp))[bn - 1],
				    ((ufs1_daddr_t *)buf_dataptr(bp))[bn]);
				    ++bn, ++*runp);
				bn = ap->in_off;
				if (runb && bn) {
					for (--bn; bn >= 0 && *runb < maxrun &&
					    is_sequential(ump,
					    ((ufs1_daddr_t *)buf_dataptr(bp))[bn],
					    ((ufs1_daddr_t *)buf_dataptr(bp))[bn+1]);
					    --bn, ++*runb);
				}
			}
			continue;
		}
		if (num == 1 && daddr && runp) {
			for (bn = ap->in_off + 1;
			    bn < MNINDIR(ump) && *runp < maxrun &&
			    is_sequential(ump,
			    ((ufs2_daddr_t *)buf_dataptr(bp))[bn - 1],
			    ((ufs2_daddr_t *)buf_dataptr(bp))[bn]);
			    ++bn, ++*runp);
			bn = ap->in_off;
			if (runb && bn) {
				for (--bn; bn >= 0 && *runb < maxrun &&
				    is_sequential(ump,
				    ((ufs2_daddr_t *)buf_dataptr(bp))[bn],
				    ((ufs2_daddr_t *)buf_dataptr(bp))[bn + 1]);
				    --bn, ++*runb);
			}
		}
	}
	if (bp)
		buf_qrelse(bp);

	/*
	 * Since this is FFS independent code, we are out of scope for the
	 * definitions of BLK_NOCOPY and BLK_SNAP, but we do know that they
	 * will fall in the range 1..um_seqinc, so we use that test and
	 * return a request for a zeroed out buffer if attempts are made
	 * to read a BLK_NOCOPY or BLK_SNAP block.
	 */
	if ((ip->i_flags & SF_SNAPSHOT) && daddr > 0 && daddr < ump->um_seqinc){
		*bnp = -1;
		return (0);
	}
	*bnp = blkptrtodb(ump, daddr);
	if (*bnp == 0) {
		if (ip->i_flags & SF_SNAPSHOT)
			*bnp = blkptrtodb(ump, bn * ump->um_seqinc);
		else
			*bnp = -1;
	}
	return (0);
}

static ufs_lbn_t
lbn_count(ump, level)
	struct ufsmount *ump;
	int level;
{
	ufs_lbn_t blockcnt;

	for (blockcnt = 1; level > 0; level--)
		blockcnt *= MNINDIR(ump);
	return (blockcnt);
}

int
ufs_bmap_seekdata(vp, offp)
	struct vnode *vp;
	off_t *offp;
{
	struct buf *bp;
	struct indir a[UFS_NIADDR + 1], *ap;
	struct inode *ip;
	struct mount *mp;
	struct ufsmount *ump;
	ufs2_daddr_t bn, daddr, nextbn;
	uint64_t bsize;
	off_t numblks;
	int error, num, num1, off;

	bp = NULL;
	error = 0;
	ip = VTOI(vp);
	mp = vnode_mount(vp);
	ump = VFSTOUFS(mp);

	if (vnode_vtype(vp) != VREG || (ip->i_flags & SF_SNAPSHOT) != 0)
		return (EINVAL);
	if (*offp < 0 || *offp >= ip->i_size)
		return (ENXIO);

	bsize = vfs_statfs(mp)->f_iosize;
	for (bn = *offp / bsize, numblks = howmany(ip->i_size, bsize);
	    bn < numblks; bn = nextbn) {
		if (bn < UFS_NDADDR) {
			daddr = DIP(ip, i_db[bn]);
			if (daddr != 0)
				break;
			nextbn = bn + 1;
			continue;
		}

		ap = a;
		error = ufs_getlbns(vp, bn, ap, &num);
		if (error != 0)
			break;
		MPASS(num >= 2);
		daddr = DIP(ip, i_ib[ap->in_off]);
        ap++;
        num--;
		for (nextbn = UFS_NDADDR, num1 = num - 1; num1 > 0; num1--)
			nextbn += lbn_count(ump, num1);
		if (daddr == 0) {
			nextbn += lbn_count(ump, num);
			continue;
		}

		for (; daddr != 0 && num > 0; ap++, num--) {
			if (bp != NULL)
				buf_qrelse(bp);
			error = readindir(vp, ap->in_lbn, daddr, &bp);
			if (error != 0)
				return (error);

			/*
			 * Scan the indirect block until we find a non-zero
			 * pointer.
			 */
			off = ap->in_off;
			do {
				daddr = I_IS_UFS1(ip) ?
				    ((ufs1_daddr_t *)buf_dataptr(bp))[off] :
				    ((ufs2_daddr_t *)buf_dataptr(bp))[off];
			} while (daddr == 0 && ++off < MNINDIR(ump));
			nextbn += off * lbn_count(ump, num - 1);

			/*
			 * We need to recompute the LBNs of indirect
			 * blocks, so restart with the updated block offset.
			 */
			if (off != ap->in_off)
				break;
		}
		if (num == 0) {
			/*
			 * We found a data block.
			 */
			bn = nextbn;
			break;
		}
	}
	if (bp != NULL)
		buf_qrelse(bp);
	if (bn >= numblks)
		error = ENXIO;
	if (error == 0 && *offp < bn * bsize)
		*offp = bn * bsize;
	return (error);
}

/*
 * Create an array of logical block number/offset pairs which represent the
 * path of indirect blocks required to access a data block.  The first "pair"
 * contains the logical block number of the appropriate single, double or
 * triple indirect block and the offset into the inode indirect block array.
 * Note, the logical block number of the inode single/double/triple indirect
 * block appears twice in the array, once with the offset into the i_ib and
 * once with the offset into the page itself.
 */
int
ufs_getlbns(vp, bn, ap, nump)
	struct vnode *vp;
	ufs2_daddr_t bn;
	struct indir *ap;
	int *nump;
{
	ufs2_daddr_t blockcnt;
	ufs_lbn_t metalbn, realbn;
	struct ufsmount *ump;
	int i, numlevels, off;

	ump = VFSTOUFS(vnode_mount(vp));
	if (nump)
		*nump = 0;
	numlevels = 0;
	realbn = bn;
	if (bn < 0)
		bn = -bn;

	/* The first UFS_NDADDR blocks are direct blocks. */
	if (bn < UFS_NDADDR)
		return (0);

	/*
	 * Determine the number of levels of indirection.  After this loop
	 * is done, blockcnt indicates the number of data blocks possible
	 * at the previous level of indirection, and UFS_NIADDR - i is the
	 * number of levels of indirection needed to locate the requested block.
	 */
	for (blockcnt = 1, i = UFS_NIADDR, bn -= UFS_NDADDR; ;
	    i--, bn -= blockcnt) {
		if (i == 0)
			return (EFBIG);
		blockcnt *= MNINDIR(ump);
		if (bn < blockcnt)
			break;
	}

	/* Calculate the address of the first meta-block. */
	if (realbn >= 0)
		metalbn = -(realbn - bn + UFS_NIADDR - i);
	else
		metalbn = -(-realbn - bn + UFS_NIADDR - i);

	/*
	 * At each iteration, off is the offset into the bap array which is
	 * an array of disk addresses at the current level of indirection.
	 * The logical block number and the offset in that block are stored
	 * into the argument array.
	 */
	ap->in_lbn = metalbn;
	ap->in_off = off = UFS_NIADDR - i;
	ap++;
	for (++numlevels; i <= UFS_NIADDR; i++) {
		/* If searching for a meta-data block, quit when found. */
		if (metalbn == realbn)
			break;

		blockcnt /= MNINDIR(ump);
		off = (int) ((bn / blockcnt) % MNINDIR(ump));

		++numlevels;
		ap->in_lbn = metalbn;
		ap->in_off = off;
		++ap;

		metalbn -= -1 + off * blockcnt;
	}
	if (nump)
		*nump = numlevels;
	return (0);
}

// SEEK_HOLE support

int
ufs_bmap_seekhole(struct vnode *vp, struct vnodeop_desc *a_desc,
                  u_long cmd, off_t *off, struct vfs_context *context)
{
    struct vnode_attr va;
    struct vnop_blockmap_args bap = {0};
    daddr_t bn, bnp;
    uint64_t bsize;
    off_t noff = 0;
    int error;

    KASSERT(off, ("Offset is null"));
    KASSERT(cmd == FSIOC_FIOSEEKHOLE || cmd == FSIOC_FIOSEEKDATA, ("Wrong command %lu", cmd));
    
    if (vn_lock(vp, LK_SHARED) != 0)
        return (EBADF);
    if (vnode_vtype(vp) != VREG) {
        error = ENOTTY;
        goto unlock;
    }
    
    if ((error = vnode_getattr(vp, &va, context)) != 0)
        goto unlock;
    
    noff = *off;
    if (noff >= va.va_data_size) {
        error = ENXIO;
        goto unlock;
    }
    
    bsize = va.va_iosize;
    
    for (bn = (daddr_t)noff / bsize; noff < va.va_data_size; bn++, noff += bsize - noff % bsize) {
        bap.a_desc = a_desc;
        bap.a_vp = vp;
        bap.a_foffset = noff;
        bap.a_size = bsize;
        bap.a_bpn = (daddr64_t*)&bnp;
        bap.a_context = context;

        error = ufs_bmap(&bap);
        if (error == EOPNOTSUPP) {
            error = ENOTTY;
            goto unlock;
        }
        if ((bnp == -1 && cmd == FSIOC_FIOSEEKHOLE) ||
            (bnp != -1 && cmd == FSIOC_FIOSEEKDATA)) {
            noff = bn * bsize;
            if (noff < *off)
                noff = *off;
            goto unlock;
        }
    }
    
    if (noff > va.va_data_size)
        noff = va.va_data_size;
    /* noff == va.va_size. There is an implicit hole at the end of file. */
    if (cmd == FSIOC_FIOSEEKDATA)
        error = ENXIO;
unlock:
    VNOP_UNLOCK(vp);
    if (error == 0)
        *off = noff;
    return (error);
}
