/*-
 * SPDX-License-Identifier: (BSD-2-Clause-FreeBSD AND BSD-3-Clause)
 *
 * Copyright (c) 2002 Networks Associates Technology, Inc.
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
 *	@(#)ffs_alloc.c	8.18 (Berkeley) 5/26/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/disk.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <stdatomic.h>
//#include <security/audit/audit.h>

#include <freebsd/compat/compat.h>
#include <freebsd/compat/taskqueue.h>

#include <ufs/ufs/dir.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ffs/softdep.h>

#include <freebsd/compat/compat.h>

extern vnode_t vfs_context_cwd(vfs_context_t);

typedef ufs2_daddr_t allocfcn_t(struct inode *ip, u_int cg, ufs2_daddr_t bpref,
				  int size, int rsize);

static ufs2_daddr_t ffs_alloccg(struct inode *, u_int, ufs2_daddr_t, int, int);
static ufs2_daddr_t
	      ffs_alloccgblk(struct inode *, struct buf *, ufs2_daddr_t, int);
static void	ffs_blkfree_cg(struct ufsmount *, struct fs *,
		    struct vnode *, ufs2_daddr_t, long, ino_t,
		    struct workhead *);
#ifdef INVARIANTS
static int	ffs_checkblk(struct inode *, ufs2_daddr_t, long);
#endif
static ufs2_daddr_t ffs_clusteralloc(struct inode *, u_int, ufs2_daddr_t, int);
static ino_t	ffs_dirpref(struct inode *);
static ufs2_daddr_t ffs_fragextend(struct inode *, u_int, ufs2_daddr_t,
		    int, int);
static ufs2_daddr_t	ffs_hashalloc
		(struct inode *, u_int, ufs2_daddr_t, int, int, allocfcn_t *);
static ufs2_daddr_t ffs_nodealloccg(struct inode *, u_int, ufs2_daddr_t, int,
		    int);
static ufs1_daddr_t ffs_mapsearch(struct fs *, struct cg *, ufs2_daddr_t, int);
static int __unused ffs_reallocblks_ufs1(struct vnop_reallocblks_args *);
static int __unused ffs_reallocblks_ufs2(struct vnop_reallocblks_args *);
static void	ffs_ckhash_cg(struct buf *);

/*
 * Allocate a block in the filesystem.
 *
 * The size of the requested block is given, which must be some
 * multiple of fs_fsize and <= fs_bsize.
 * A preference may be optionally specified. If a preference is given
 * the following hierarchy is used to allocate a block:
 *   1) allocate the requested block.
 *   2) allocate a rotationally optimal block in the same cylinder.
 *   3) allocate a block in the same cylinder group.
 *   4) quadradically rehash into other cylinder groups, until an
 *      available block is located.
 * If no block preference is given the following hierarchy is used
 * to allocate a block:
 *   1) allocate a block in the cylinder group that contains the
 *      inode for the file.
 *   2) quadradically rehash into other cylinder groups, until an
 *      available block is located.
 */
int
ffs_alloc(struct inode *ip, ufs2_daddr_t lbn, ufs2_daddr_t bpref,
          int size, int flags, struct ucred *cred, ufs2_daddr_t *bnp)
{
	struct fs *fs;
	struct ufsmount *ump;
	ufs2_daddr_t bno;
	u_int cg, reclaimed;
	int64_t delta;
    
#ifdef QUOTA
	int error;
#endif

	*bnp = 0;
	ump = ITOUMP(ip);
	fs = ump->um_fs;
	lck_mtx_assert(UFS_MTX(ump), LCK_MTX_ASSERT_OWNED);
#ifdef INVARIANTS
	if ((u_int)size > fs->fs_bsize || fragoff(fs, size) != 0) {
		log_debug("dev = %s, bsize = %ld, size = %d, fs = %s\n",
		    devtoname(ump->um_devvp), (long)fs->fs_bsize, size,
		    fs->fs_fsmnt);
		panic("ffs_alloc: bad size");
	}
	if (cred == NOCRED)
		panic("ffs_alloc: missing credential");
#endif /* INVARIANTS */
	reclaimed = 0;
retry:
#ifdef QUOTA
	UFS_UNLOCK(ump);
	error = chkdq(ip, btodb(size, ump->um_devbsize), cred, 0);
	if (error)
		return (error);
	UFS_LOCK(ump);
#endif
	if (size == fs->fs_bsize && fs->fs_cstotal.cs_nbfree == 0)
		goto nospace;
	if (freespace(fs, fs->fs_minfree) - numfrags(fs, size) < 0)
		goto nospace;
	if (bpref >= fs->fs_size)
		bpref = 0;
	if (bpref == 0)
		cg = ino_to_cg(fs, ip->i_number);
	else
		cg = (int)dtog(fs, bpref);
	bno = ffs_hashalloc(ip, cg, bpref, size, size, ffs_alloccg);
	if (bno > 0) {
		delta = btodb(size, ump->um_devbsize);
		DIP_SET(ip, i_blocks, DIP(ip, i_blocks) + delta);
		if (flags & FREEBSD_IO_EXT)
			UFS_INODE_SET_FLAG(ip, IN_CHANGE);
		else
			UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
		*bnp = bno;
		return (0);
	}
nospace:
#ifdef QUOTA
	UFS_UNLOCK(ump);
	/*
	 * Restore user's disk quota because allocation failed.
	 */
	(void) chkdq(ip, -btodb(size, ump->um_devbsize), cred, FORCE);
	UFS_LOCK(ump);
#endif
	if (reclaimed == 0 && (flags & FREEBSD_IO_BUFLOCKED) == 0) {
		reclaimed = 1;
		softdep_request_cleanup(fs, ITOV(ip), cred, FLUSH_BLOCKS_WAIT);
		goto retry;
	}
	if (ffs_fsfail_cleanup_locked(ump, 0)) {
		UFS_UNLOCK(ump);
		return (ENXIO);
	}
	if (reclaimed > 0 &&
	    ppsratecheck(&ump->um_last_fullmsg, &ump->um_secs_fullmsg, 1)) {
		UFS_UNLOCK(ump);
		ffs_fserr(fs, ip->i_number, "filesystem full");
		log_debug("\n%s: write failed, filesystem is full\n",
		    fs->fs_fsmnt);
	} else {
		UFS_UNLOCK(ump);
	}
	return (ENOSPC);
}

/*
 * Reallocate a fragment to a bigger size
 *
 * The number and size of the old block is given, and a preference
 * and new size is also specified. The allocator attempts to extend
 * the original block. Failing that, the regular block allocator is
 * invoked to get an appropriate block.
 */
int
ffs_realloccg(struct inode *ip, ufs2_daddr_t lbprev, ufs2_daddr_t bprev, ufs2_daddr_t bpref, int osize, int nsize, int flags, struct ucred *cred, struct buf **bpp)
{
	struct vnode *vp;
	struct fs *fs;
	struct buf *bp;
	struct ufsmount *ump;
	u_int cg, request, reclaimed;
	int error, gbflags;
	ufs2_daddr_t bno;
	int64_t delta;

	vp = ITOV(ip);
	ump = ITOUMP(ip);
	fs = ump->um_fs;
	bp = NULL;
	gbflags = (flags & BA_UNMAPPED) != 0 ? GB_UNMAPPED : 0;

	lck_mtx_assert(UFS_MTX(ump), LCK_MTX_ASSERT_OWNED);
#ifdef INVARIANTS
	if (vnode_mount(vp)->mnt_kern_flag & MNTK_SUSPENDED)
		panic("ffs_realloccg: allocation on suspended filesystem");
	if ((u_int)osize > fs->fs_bsize || fragoff(fs, osize) != 0 ||
	    (u_int)nsize > fs->fs_bsize || fragoff(fs, nsize) != 0) {
		log_debug(
		"dev = %s, bsize = %ld, osize = %d, nsize = %d, fs = %s\n",
		    devtoname(ump->um_devvp), (long)fs->fs_bsize, osize,
		    nsize, fs->fs_fsmnt);
		panic("ffs_realloccg: bad size");
	}
	if (cred == NOCRED)
		panic("ffs_realloccg: missing credential");
#endif /* INVARIANTS */
	reclaimed = 0;
retry:
	if (freespace(fs, fs->fs_minfree) -  numfrags(fs, nsize - osize) < 0) {
		goto nospace;
	}
	if (bprev == 0) {
		log_debug("dev = %s, bsize = %ld, bprev = %lld, fs = %s\n",
		    devtoname(ump->um_devvp), (long)fs->fs_bsize, (intmax_t)bprev,
		    fs->fs_fsmnt);
		panic("ffs_realloccg: bad bprev");
	}
	UFS_UNLOCK(ump);
	/*
	 * Allocate the extra space in the buffer.
	 */
	error = bread(vp, lbprev, osize, NOCRED, 0, &bp);
	if (error) {
		return (error);
	}

	if (buf_blkno(bp) == buf_lblkno(bp)) {
		if (lbprev >= UFS_NDADDR)
			panic("ffs_realloccg: lbprev out of range");
		buf_setlblkno(bp, fsbtodb(fs, bprev));
	}

#ifdef QUOTA
	error = chkdq(ip, btodb(nsize - osize, ump->um_devbsize), cred, 0);
	if (error) {
		buf_brelse(bp);
		return (error);
	}
#endif
	/*
	 * Check for extension in the existing location.
	 */
	*bpp = NULL;
	cg = (int) dtog(fs, bprev);
	UFS_LOCK(ump);
	bno = ffs_fragextend(ip, cg, bprev, osize, nsize);
	if (bno) {
		if (buf_blkno(bp) != fsbtodb(fs, bno))
			panic("ffs_realloccg: bad blockno");
		delta = btodb(nsize - osize, ump->um_devbsize);
		DIP_SET(ip, i_blocks, DIP(ip, i_blocks) + delta);
		if (flags & FREEBSD_IO_EXT)
			UFS_INODE_SET_FLAG(ip, IN_CHANGE);
		else
			UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
// FIXME: todo...
#if 0
		allocbuf(bp, nsize);
		buf_setflags(bp, B_DONE);
		vfs_bio_bzero_buf(bp, osize, nsize - osize);
		if ((buf_flags(bp) & (B_MALLOC | B_VMIO)) == B_VMIO)
			vfs_bio_set_valid(bp, osize, nsize - osize);
#endif
		*bpp = bp;
		return (0);
	}
	/*
	 * Allocate a new disk location.
	 */
	if (bpref >= fs->fs_size)
		bpref = 0;
	switch ((int)fs->fs_optim) {
	case FS_OPTSPACE:
		/*
		 * Allocate an exact sized fragment. Although this makes
		 * best use of space, we will waste time relocating it if
		 * the file continues to grow. If the fragmentation is
		 * less than half of the minimum free reserve, we choose
		 * to begin optimizing for time.
		 */
		request = nsize;
		if (fs->fs_minfree <= 5 ||
		    fs->fs_cstotal.cs_nffree >
		    (off_t)fs->fs_dsize * fs->fs_minfree / (2 * 100))
			break;
		log(LOG_NOTICE, "%s: optimization changed from SPACE to TIME\n",
			fs->fs_fsmnt);
		fs->fs_optim = FS_OPTTIME;
		break;
	case FS_OPTTIME:
		/*
		 * At this point we have discovered a file that is trying to
		 * grow a small fragment to a larger fragment. To save time,
		 * we allocate a full sized block, then free the unused portion.
		 * If the file continues to grow, the `ffs_fragextend' call
		 * above will be able to grow it in place without further
		 * copying. If aberrant programs cause disk fragmentation to
		 * grow within 2% of the free reserve, we choose to begin
		 * optimizing for space.
		 */
		request = fs->fs_bsize;
		if (fs->fs_cstotal.cs_nffree <
		    (off_t)fs->fs_dsize * (fs->fs_minfree - 2) / 100)
			break;
		log(LOG_NOTICE, "%s: optimization changed from TIME to SPACE\n",
			fs->fs_fsmnt);
		fs->fs_optim = FS_OPTSPACE;
		break;
	default:
		log_debug("dev = %s, optim = %ld, fs = %s\n",
		    devtoname(ump->um_devvp), (long)fs->fs_optim, fs->fs_fsmnt);
		panic("ffs_realloccg: bad optim");
        __builtin_unreachable();
	}
	bno = ffs_hashalloc(ip, cg, bpref, request, nsize, ffs_alloccg);
	if (bno > 0) {
		buf_setlblkno(bp, fsbtodb(fs, bno));
		if (!DOINGSOFTDEP(vp))
			/*
			 * The usual case is that a smaller fragment that
			 * was just allocated has been replaced with a bigger
			 * fragment or a full-size block. If it is marked as
			 * B_DELWRI, the current contents have not been written
			 * to disk. It is possible that the block was written
			 * earlier, but very uncommon. If the block has never
			 * been written, there is no need to send a BIO_DELETE
			 * for it when it is freed. The gain from avoiding the
			 * TRIMs for the common case of unwritten blocks far
			 * exceeds the cost of the write amplification for the
			 * uncommon case of failing to send a TRIM for a block
			 * that had been written.
			 */
			ffs_blkfree(ump, fs, ump->um_devvp, bprev, (long)osize,
			    ip->i_number, vnode_vtype(vp), NULL,
			    (buf_flags(bp) & B_DELWRI) != 0 ?
			    NOTRIM_KEY : SINGLETON_KEY);
		delta = btodb(nsize - osize, ump->um_devbsize);
		DIP_SET(ip, i_blocks, DIP(ip, i_blocks) + delta);
		if (flags & FREEBSD_IO_EXT)
			UFS_INODE_SET_FLAG(ip, IN_CHANGE);
		else
			UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
// FIXME: todo...
#if 0
		allocbuf(bp, nsize);
		buf_setflags(bp, B_DONE);
		vfs_bio_bzero_buf(bp, osize, nsize - osize);
		if ((buf_flags(bp) & (Bmalloc | B_VMIO)) == B_VMIO)
			vfs_bio_set_valid(bp, osize, nsize - osize);
#endif
		*bpp = bp;
		return (0);
	}
#ifdef QUOTA
	UFS_UNLOCK(ump);
	/*
	 * Restore user's disk quota because allocation failed.
	 */
	(void) chkdq(ip, -btodb(nsize - osize, ump->um_devbsize), cred, FORCE);
	UFS_LOCK(ump);
#endif
nospace:
	/*
	 * no space available
	 */
	if (reclaimed == 0 && (flags & FREEBSD_IO_BUFLOCKED) == 0) {
		reclaimed = 1;
		UFS_UNLOCK(ump);
		if (bp) {
			buf_brelse(bp);
			bp = NULL;
		}
		UFS_LOCK(ump);
		softdep_request_cleanup(fs, vp, cred, FLUSH_BLOCKS_WAIT);
		goto retry;
	}
	if (bp)
		buf_brelse(bp);
	if (ffs_fsfail_cleanup_locked(ump, 0)) {
		UFS_UNLOCK(ump);
		return (ENXIO);
	}
	if (reclaimed > 0 &&
	    ppsratecheck(&ump->um_last_fullmsg, &ump->um_secs_fullmsg, 1)) {
		UFS_UNLOCK(ump);
		ffs_fserr(fs, ip->i_number, "filesystem full");
		log_debug("\n%s: write failed, filesystem is full\n",
		    fs->fs_fsmnt);
	} else {
		UFS_UNLOCK(ump);
	}
	return (ENOSPC);
}

/*
 * Reallocate a sequence of blocks into a contiguous sequence of blocks.
 *
 * The vnode and an array of buffer pointers for a range of sequential
 * logical blocks to be made contiguous is given. The allocator attempts
 * to find a range of sequential blocks starting as close as possible
 * from the end of the allocation for the logical block immediately
 * preceding the current range. If successful, the physical block numbers
 * in the buffer pointers and in the inode are changed to reflect the new
 * allocation. If unsuccessful, the allocation is left unchanged. The
 * success in doing the reallocation is returned. Note that the error
 * return is not reflected back to the user. Rather the previous block
 * allocation will be used.
 */

SYSCTL_NODE(_vfs, OID_AUTO, ffs, CTLFLAG_RW, 0,
    "FFS filesystem");

static int doasyncfree = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, doasyncfree, CTLFLAG_RW, &doasyncfree, 0,
"do not force synchronous writes when blocks are reallocated");

static int doreallocblks = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, doreallocblks, CTLFLAG_RW, &doreallocblks, 0,
"enable block reallocation");

static int dotrimcons = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, dotrimcons, CTLFLAG_RW, &dotrimcons, 0,
"enable BIO_DELETE / TRIM consolidation");

static int maxclustersearch = 10;
SYSCTL_INT(_vfs_ffs, OID_AUTO, maxclustersearch, CTLFLAG_RW, &maxclustersearch,
0, "max number of cylinder group to search for contigous blocks");

#ifdef DIAGNOSTIC
static int prtrealloc = 0;
SYSCTL_INT(_debug, OID_AUTO, ffs_prtrealloc, CTLFLAG_RW, &prtrealloc, 0,
	"print out FFS filesystem block reallocation operations");
#endif

int
ffs_reallocblks(struct vnop_reallocblks_args *ap)
    /* {
		struct vnode *a_vp;
		struct cluster_save *a_buflist;
	} */
{
#if REALLOC_BLOCKS
	struct ufsmount *ump;

	/*
	 * We used to skip reallocating the blocks of a file into a
	 * contiguous sequence if the underlying flash device requested
	 * BIO_DELETE notifications, because devices that benefit from
	 * BIO_DELETE also benefit from not moving the data. However,
	 * the destination for the data is usually moved before the data
	 * is written to the initially allocated location, so we rarely
	 * suffer the penalty of extra writes. With the addition of the
	 * consolidation of contiguous blocks into single BIO_DELETE
	 * operations, having fewer but larger contiguous blocks reduces
	 * the number of (slow and expensive) BIO_DELETE operations. So
	 * when doing BIO_DELETE consolidation, we do block reallocation.
	 *
	 * Skip if reallocblks has been disabled globally.
	 */
	ump = VFSTOUFS(vnode_mount(ap->a_vp));
	if ((((ump->um_flags) & UM_CANDELETE) != 0 && dotrimcons == 0) ||
	    doreallocblks == 0)
		return (ENOSPC);

	/*
	 * We can't wait in softdep prealloc as it may fsync and recurse
	 * here.  Instead we simply fail to reallocate blocks if this
	 * rare condition arises.
	 */
	if (DOINGSOFTDEP(ap->a_vp))
		if (softdep_prealloc(ap->a_vp, MNT_NOWAIT) != 0)
			return (ENOSPC);
	if (ump->um_fstype == UFS1)
		return (ffs_reallocblks_ufs1(ap));
	return (ffs_reallocblks_ufs2(ap));
#endif
    return ENOTSUP;
}

#ifdef REALLOC_BLOCKS

static int
ffs_reallocblks_ufs1(struct vnop_reallocblks_args *ap)
    /* {
		struct vnode *a_vp;
		struct cluster_save *a_buflist;
	} */
{
	struct fs *fs;
	struct inode *ip;
	struct vnode *vp;
	struct buf *sbp, *ebp, *bp;
	ufs1_daddr_t *bap, *sbap, *ebap;
	struct cluster_save *buflist;
	struct ufsmount *ump;
	ufs_lbn_t start_lbn, end_lbn;
	ufs1_daddr_t soff, newblk, blkno;
	ufs2_daddr_t pref;
	struct indir start_ap[UFS_NIADDR + 1], end_ap[UFS_NIADDR + 1], *idp;
	int i, cg, len, start_lvl, end_lvl, ssize;

	vp = ap->a_vp;
	ip = VTOI(vp);
	ump = ITOUMP(ip);
	fs = ump->um_fs;
	/*
	 * If we are not tracking block clusters or if we have less than 4%
	 * free blocks left, then do not attempt to cluster. Running with
	 * less than 5% free block reserve is not recommended and those that
	 * choose to do so do not expect to have good file layout.
	 */
	if (fs->fs_contigsumsize <= 0 || freespace(fs, 4) < 0)
		return (ENOSPC);
	buflist = ap->a_buflist;
	len = buflist->bs_nchildren;
	start_lbn = buf_lblkno(buflist->bs_children[0]);
	end_lbn = start_lbn + len - 1;
#ifdef INVARIANTS
	for (i = 0; i < len; i++)
		if (!ffs_checkblk(ip,
		   dbtofsb(fs,buf_blkno( buflist->bs_children[i])), fs->fs_bsize))
			panic("ffs_reallocblks: unallocated block 1");
	for (i = 1; i < len; i++)
		if (buf_lblkno(buflist->bs_children[i]) != start_lbn + i)
			panic("ffs_reallocblks: non-logical cluster");
	blkno =buf_blkno( buflist->bs_children[0]);
	ssize = fsbtodb(fs, fs->fs_frag);
	for (i = 1; i < len - 1; i++)
		if (buf_blkno(buflist->bs_children[i]) != blkno + (i * ssize))
			panic("ffs_reallocblks: non-physical cluster %d", i);
#endif
	/*
	 * If the cluster crosses the boundary for the first indirect
	 * block, leave space for the indirect block. Indirect blocks
	 * are initially laid out in a position after the last direct
	 * block. Block reallocation would usually destroy locality by
	 * moving the indirect block out of the way to make room for
	 * data blocks if we didn't compensate here. We should also do
	 * this for other indirect block boundaries, but it is only
	 * important for the first one.
	 */
	if (start_lbn < UFS_NDADDR && end_lbn >= UFS_NDADDR)
		return (ENOSPC);
	/*
	 * If the latest allocation is in a new cylinder group, assume that
	 * the filesystem has decided to move and do not force it back to
	 * the previous cylinder group.
	 */
	if (dtog(fs, dbtofsb(fs,buf_blkno( buflist->bs_children[0]))) !=
	    dtog(fs, dbtofsb(fs,buf_blkno( buflist->bs_children[len - 1]))))
		return (ENOSPC);
	if (ufs_getlbns(vp, start_lbn, start_ap, &start_lvl) ||
	    ufs_getlbns(vp, end_lbn, end_ap, &end_lvl))
		return (ENOSPC);
	/*
	 * Get the starting offset and block map for the first block.
	 */
	if (start_lvl == 0) {
		sbap = &ip->i_din1->di_db[0];
		soff = start_lbn;
	} else {
		idp = &start_ap[start_lvl - 1];
		if (buf_bread(vp, idp->in_lbn, (int)fs->fs_bsize, NOCRED, &sbp)) {
			buf_brelse(sbp);
			return (ENOSPC);
		}
		sbap = (ufs1_daddr_t *)buf_dataptr(sbp);
		soff = idp->in_off;
	}
	/*
	 * If the block range spans two block maps, get the second map.
	 */
	ebap = NULL;
	if (end_lvl == 0 || (idp = &end_ap[end_lvl - 1])->in_off + 1 >= len) {
		ssize = len;
	} else {
#ifdef INVARIANTS
		if (start_lvl > 0 &&
		    start_ap[start_lvl - 1].in_lbn == idp->in_lbn)
			panic("ffs_reallocblk: start == end");
#endif
		ssize = len - (idp->in_off + 1);
		if (buf_bread(vp, idp->in_lbn, (int)fs->fs_bsize, NOCRED, &ebp))
			goto fail;
		ebap = (ufs1_daddr_t *)buf_dataptr(ebp);
	}
	/*
	 * Find the preferred location for the cluster. If we have not
	 * previously failed at this endeavor, then follow our standard
	 * preference calculation. If we have failed at it, then pick up
	 * where we last ended our search.
	 */
	UFS_LOCK(ump);
	if (ip->i_nextclustercg == -1)
		pref = ffs_blkpref_ufs1(ip, start_lbn, soff, sbap);
	else
		pref = cgdata(fs, ip->i_nextclustercg);
	/*
	 * Search the block map looking for an allocation of the desired size.
	 * To avoid wasting too much time, we limit the number of cylinder
	 * groups that we will search.
	 */
	cg = dtog(fs, pref);
	for (i = min(maxclustersearch, fs->fs_ncg); i > 0; i--) {
		if ((newblk = ffs_clusteralloc(ip, cg, pref, len)) != 0)
			break;
		cg += 1;
		if (cg >= fs->fs_ncg)
			cg = 0;
	}
	/*
	 * If we have failed in our search, record where we gave up for
	 * next time. Otherwise, fall back to our usual search citerion.
	 */
	if (newblk == 0) {
		ip->i_nextclustercg = cg;
		UFS_UNLOCK(ump);
		goto fail;
	}
	ip->i_nextclustercg = -1;
	/*
	 * We have found a new contiguous block.
	 *
	 * First we have to replace the old block pointers with the new
	 * block pointers in the inode and indirect blocks associated
	 * with the file.
	 */
#ifdef DIAGNOSTIC
	if (prtrealloc)
		log_debug("realloc: ino %llu, lbns %lld-%lld\n\told:",
		    (uintmax_t)ip->i_number,
		    (intmax_t)start_lbn, (intmax_t)end_lbn);
#endif
	blkno = newblk;
	for (bap = &sbap[soff], i = 0; i < len; i++, blkno += fs->fs_frag) {
		if (i == ssize) {
			bap = ebap;
			soff = -i;
		}
#ifdef INVARIANTS
		if (!ffs_checkblk(ip,
		   dbtofsb(fs,buf_blkno( buflist->bs_children[i])), fs->fs_bsize))
			panic("ffs_reallocblks: unallocated block 2");
		if (dbtofsb(fs,buf_blkno( buflist->bs_children[i])) != *bap)
			panic("ffs_reallocblks: alloc mismatch");
#endif
#ifdef DIAGNOSTIC
		if (prtrealloc)
			log_debug(" %d,", *bap);
#endif
		if (DOINGSOFTDEP(vp)) {
			if (sbap == &ip->i_din1->di_db[0] && i < ssize)
				softdep_setup_allocdirect(ip, start_lbn + i,
				    blkno, *bap, fs->fs_bsize, fs->fs_bsize,
				    buflist->bs_children[i]);
			else
				softdep_setup_allocindir_page(ip, start_lbn + i,
				    i < ssize ? sbp : ebp, soff + i, blkno,
				    *bap, buflist->bs_children[i]);
		}
		*bap++ = blkno;
	}
	/*
	 * Next we must write out the modified inode and indirect blocks.
	 * For strict correctness, the writes should be synchronous since
	 * the old block values may have been written to disk. In practise
	 * they are almost never written, but if we are concerned about
	 * strict correctness, the `doasyncfree' flag should be set to zero.
	 *
	 * The test on `doasyncfree' should be changed to test a flag
	 * that shows whether the associated buffers and inodes have
	 * been written. The flag should be set when the cluster is
	 * started and cleared whenever the buffer or inode is flushed.
	 * We can then check below to see if it is set, and do the
	 * synchronous write only when it has been cleared.
	 */
	if (sbap != &ip->i_din1->di_db[0]) {
		if (doasyncfree)
			buf_bdwrite(sbp);
		else
			bwrite(sbp);
	} else {
		UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
		if (!doasyncfree)
			ffs_update(vp, 1);
	}
	if (ssize < len) {
		if (doasyncfree)
			buf_bdwrite(ebp);
		else
			bwrite(ebp);
	}
	/*
	 * Last, free the old blocks and assign the new blocks to the buffers.
	 */
#ifdef DIAGNOSTIC
	if (prtrealloc)
		log_debug("\n\tnew:");
#endif
	for (blkno = newblk, i = 0; i < len; i++, blkno += fs->fs_frag) {
		bp = buflist->bs_children[i];
		if (!DOINGSOFTDEP(vp))
			/*
			 * The usual case is that a set of N-contiguous blocks
			 * that was just allocated has been replaced with a
			 * set of N+1-contiguous blocks. If they are marked as
			 * B_DELWRI, the current contents have not been written
			 * to disk. It is possible that the blocks were written
			 * earlier, but very uncommon. If the blocks have never
			 * been written, there is no need to send a BIO_DELETE
			 * for them when they are freed. The gain from avoiding
			 * the TRIMs for the common case of unwritten blocks
			 * far exceeds the cost of the write amplification for
			 * the uncommon case of failing to send a TRIM for the
			 * blocks that had been written.
			 */
			ffs_blkfree(ump, fs, ump->um_devvp,
			    dbtofsb(fs,buf_blkno( bp)),
			    fs->fs_bsize, ip->i_number, vnode_vtype(vp), NULL,
			    (buf_flags(bp) & B_DELWRI) != 0 ?
			    NOTRIM_KEY : SINGLETON_KEY);
		buf_setlblkno(bp, fsbtodb(fs, blkno));
#ifdef INVARIANTS
		if (!ffs_checkblk(ip, dbtofsb(fs,buf_blkno( bp)), fs->fs_bsize))
			panic("ffs_reallocblks: unallocated block 3");
#endif
#ifdef DIAGNOSTIC
		if (prtrealloc)
			log_debug(" %d,", blkno);
#endif
	}
#ifdef DIAGNOSTIC
	if (prtrealloc) {
		prtrealloc--;
		log_debug("\n");
	}
#endif
	return (0);

fail:
	if (ssize < len)
		buf_brelse(ebp);
	if (sbap != &ip->i_din1->di_db[0])
		buf_brelse(sbp);
    return (ENOSPC);
}

static int
ffs_reallocblks_ufs2(struct vnop_reallocblks_args *ap)
    /* {
		struct vnode *a_vp;
		struct cluster_save *a_buflist;
	} */
{
#if 0
	struct fs *fs;
	struct inode *ip;
	struct vnode *vp;
	struct buf *sbp, *ebp, *bp;
	ufs2_daddr_t *bap, *sbap, *ebap;
	struct cluster_save *buflist;
	struct ufsmount *ump;
	ufs_lbn_t start_lbn, end_lbn;
	ufs2_daddr_t soff, newblk, blkno, pref;
	struct indir start_ap[UFS_NIADDR + 1], end_ap[UFS_NIADDR + 1], *idp;
	int i, cg, len, start_lvl, end_lvl, ssize;

	vp = ap->a_vp;
	ip = VTOI(vp);
	ump = ITOUMP(ip);
	fs = ump->um_fs;
	/*
	 * If we are not tracking block clusters or if we have less than 4%
	 * free blocks left, then do not attempt to cluster. Running with
	 * less than 5% free block reserve is not recommended and those that
	 * choose to do so do not expect to have good file layout.
	 */
	if (fs->fs_contigsumsize <= 0 || freespace(fs, 4) < 0)
		return (ENOSPC);
	buflist = ap->a_buflist;
	len = buflist->bs_nchildren;
	start_lbn = buf_lblkno(buflist->bs_children[0]);
	end_lbn = start_lbn + len - 1;
#ifdef INVARIANTS
	for (i = 0; i < len; i++)
		if (!ffs_checkblk(ip,
		   dbtofsb(fs,buf_blkno( buflist->bs_children[i])), fs->fs_bsize))
			panic("ffs_reallocblks: unallocated block 1");
	for (i = 1; i < len; i++)
		if (buf_lblkno(buflist->bs_children[i]) != start_lbn + i)
			panic("ffs_reallocblks: non-logical cluster");
	blkno =buf_blkno( buflist->bs_children[0]);
	ssize = fsbtodb(fs, fs->fs_frag);
	for (i = 1; i < len - 1; i++)
		if (buf_blkno(buflist->bs_children[i]) != blkno + (i * ssize))
			panic("ffs_reallocblks: non-physical cluster %d", i);
#endif
	/*
	 * If the cluster crosses the boundary for the first indirect
	 * block, do not move anything in it. Indirect blocks are
	 * usually initially laid out in a position between the data
	 * blocks. Block reallocation would usually destroy locality by
	 * moving the indirect block out of the way to make room for
	 * data blocks if we didn't compensate here. We should also do
	 * this for other indirect block boundaries, but it is only
	 * important for the first one.
	 */
	if (start_lbn < UFS_NDADDR && end_lbn >= UFS_NDADDR)
		return (ENOSPC);
	/*
	 * If the latest allocation is in a new cylinder group, assume that
	 * the filesystem has decided to move and do not force it back to
	 * the previous cylinder group.
	 */
	if (dtog(fs, dbtofsb(fs, buf_blkno(buflist->bs_children[0]))) !=
	    dtog(fs, dbtofsb(fs, buf_blkno(buflist->bs_children[len - 1]))))
		return (ENOSPC);
	if (ufs_getlbns(vp, start_lbn, start_ap, &start_lvl) ||
	    ufs_getlbns(vp, end_lbn, end_ap, &end_lvl))
		return (ENOSPC);
	/*
	 * Get the starting offset and block map for the first block.
	 */
	if (start_lvl == 0) {
		sbap = &ip->i_din2->di_db[0];
		soff = start_lbn;
	} else {
		idp = &start_ap[start_lvl - 1];
		if (buf_bread(vp, idp->in_lbn, (int)fs->fs_bsize, NOCRED, &sbp)) {
			buf_brelse(sbp);
			return (ENOSPC);
		}
		sbap = (ufs2_daddr_t *)buf_dataptr(sbp);
		soff = idp->in_off;
	}
	/*
	 * If the block range spans two block maps, get the second map.
	 */
	ebap = NULL;
	if (end_lvl == 0 || (idp = &end_ap[end_lvl - 1])->in_off + 1 >= len) {
		ssize = len;
	} else {
#ifdef INVARIANTS
		if (start_lvl > 0 &&
		    start_ap[start_lvl - 1].in_lbn == idp->in_lbn)
			panic("ffs_reallocblk: start == end");
#endif
		ssize = len - (idp->in_off + 1);
		if (buf_bread(vp, idp->in_lbn, (int)fs->fs_bsize, NOCRED, &ebp))
			goto fail;
		ebap = (ufs2_daddr_t *)buf_dataptr(ebp);
	}
	/*
	 * Find the preferred location for the cluster. If we have not
	 * previously failed at this endeavor, then follow our standard
	 * preference calculation. If we have failed at it, then pick up
	 * where we last ended our search.
	 */
	UFS_LOCK(ump);
	if (ip->i_nextclustercg == -1)
		pref = ffs_blkpref_ufs2(ip, start_lbn, soff, sbap);
	else
		pref = cgdata(fs, ip->i_nextclustercg);
	/*
	 * Search the block map looking for an allocation of the desired size.
	 * To avoid wasting too much time, we limit the number of cylinder
	 * groups that we will search.
	 */
	cg = dtog(fs, pref);
	for (i = min(maxclustersearch, fs->fs_ncg); i > 0; i--) {
		if ((newblk = ffs_clusteralloc(ip, cg, pref, len)) != 0)
			break;
		cg += 1;
		if (cg >= fs->fs_ncg)
			cg = 0;
	}
	/*
	 * If we have failed in our search, record where we gave up for
	 * next time. Otherwise, fall back to our usual search citerion.
	 */
	if (newblk == 0) {
		ip->i_nextclustercg = cg;
		UFS_UNLOCK(ump);
		goto fail;
	}
	ip->i_nextclustercg = -1;
	/*
	 * We have found a new contiguous block.
	 *
	 * First we have to replace the old block pointers with the new
	 * block pointers in the inode and indirect blocks associated
	 * with the file.
	 */
#ifdef DIAGNOSTIC
	if (prtrealloc)
		log_debug("realloc: ino %llu, lbns %lld-%lld\n\told:", (uintmax_t)ip->i_number,
		    (intmax_t)start_lbn, (intmax_t)end_lbn);
#endif
	blkno = newblk;
	for (bap = &sbap[soff], i = 0; i < len; i++, blkno += fs->fs_frag) {
		if (i == ssize) {
			bap = ebap;
			soff = -i;
		}
#ifdef INVARIANTS
		if (!ffs_checkblk(ip,
		   dbtofsb(fs,buf_blkno( buflist->bs_children[i])), fs->fs_bsize))
			panic("ffs_reallocblks: unallocated block 2");
		if (dbtofsb(fs,buf_blkno( buflist->bs_children[i])) != *bap)
			panic("ffs_reallocblks: alloc mismatch");
#endif
#ifdef DIAGNOSTIC
		if (prtrealloc)
			log_debug(" %lld,", (intmax_t)*bap);
#endif
		if (DOINGSOFTDEP(vp)) {
			if (sbap == &ip->i_din2->di_db[0] && i < ssize)
				softdep_setup_allocdirect(ip, start_lbn + i,
				    blkno, *bap, fs->fs_bsize, fs->fs_bsize,
				    buflist->bs_children[i]);
			else
				softdep_setup_allocindir_page(ip, start_lbn + i,
				    i < ssize ? sbp : ebp, soff + i, blkno,
				    *bap, buflist->bs_children[i]);
		}
		*bap++ = blkno;
	}
	/*
	 * Next we must write out the modified inode and indirect blocks.
	 * For strict correctness, the writes should be synchronous since
	 * the old block values may have been written to disk. In practise
	 * they are almost never written, but if we are concerned about
	 * strict correctness, the `doasyncfree' flag should be set to zero.
	 *
	 * The test on `doasyncfree' should be changed to test a flag
	 * that shows whether the associated buffers and inodes have
	 * been written. The flag should be set when the cluster is
	 * started and cleared whenever the buffer or inode is flushed.
	 * We can then check below to see if it is set, and do the
	 * synchronous write only when it has been cleared.
	 */
	if (sbap != &ip->i_din2->di_db[0]) {
		if (doasyncfree)
			buf_bdwrite(sbp);
		else
			bwrite(sbp);
	} else {
		UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_UPDATE);
		if (!doasyncfree)
			ffs_update(vp, 1);
	}
	if (ssize < len) {
		if (doasyncfree)
			buf_bdwrite(ebp);
		else
			bwrite(ebp);
	}
	/*
	 * Last, free the old blocks and assign the new blocks to the buffers.
	 */
#ifdef DIAGNOSTIC
	if (prtrealloc)
		log_debug("\n\tnew:");
#endif
	for (blkno = newblk, i = 0; i < len; i++, blkno += fs->fs_frag) {
		bp = buflist->bs_children[i];
		if (!DOINGSOFTDEP(vp))
			/*
			 * The usual case is that a set of N-contiguous blocks
			 * that was just allocated has been replaced with a
			 * set of N+1-contiguous blocks. If they are marked as
			 * B_DELWRI, the current contents have not been written
			 * to disk. It is possible that the blocks were written
			 * earlier, but very uncommon. If the blocks have never
			 * been written, there is no need to send a BIO_DELETE
			 * for them when they are freed. The gain from avoiding
			 * the TRIMs for the common case of unwritten blocks
			 * far exceeds the cost of the write amplification for
			 * the uncommon case of failing to send a TRIM for the
			 * blocks that had been written.
			 */
			ffs_blkfree(ump, fs, ump->um_devvp,
			    dbtofsb(fs,buf_blkno( bp)),
			    fs->fs_bsize, ip->i_number, vnode_vtype(vp), NULL,
			    (buf_flags(bp) & B_DELWRI) != 0 ?
			    NOTRIM_KEY : SINGLETON_KEY);
		buf_setlblkno(bp, fsbtodb(fs, blkno));
#ifdef INVARIANTS
		if (!ffs_checkblk(ip, dbtofsb(fs,buf_blkno( bp)), fs->fs_bsize))
			panic("ffs_reallocblks: unallocated block 3");
#endif
#ifdef DIAGNOSTIC
		if (prtrealloc)
			log_debug(" %lld,", (intmax_t)blkno);
#endif
	}
#ifdef DIAGNOSTIC
	if (prtrealloc) {
		prtrealloc--;
		log_debug("\n");
	}
#endif
	return (0);

fail:
	if (ssize < len)
		buf_brelse(ebp);
	if (sbap != &ip->i_din2->di_db[0])
		buf_brelse(sbp);
#endif
	return (ENOSPC);
}

#endif /* REALLOC_BLOCKS */

/*
 * Allocate an inode in the filesystem.
 *
 * If allocating a directory, use ffs_dirpref to select the inode.
 * If allocating in a directory, the following hierarchy is followed:
 *   1) allocate the preferred inode.
 *   2) allocate an inode in the same cylinder group.
 *   3) quadradically rehash into other cylinder groups, until an
 *      available inode is located.
 * If no inode preference is given the following hierarchy is used
 * to allocate an inode:
 *   1) allocate an inode in cylinder group 0.
 *   2) quadradically rehash into other cylinder groups, until an
 *      available inode is located.
 */
int
ffs_valloc(struct vnode *pvp, int mode, struct vfs_context *ctx, struct vnode **vpp)
{
	struct inode *pip;
	struct fs *fs;
	struct inode *ip;
	struct timespec ts;
	struct ufsmount *ump;
    struct ucred *cred;
	ino_t ino, ipref;
	u_int cg;
	int error, reclaimed;
    struct vfs_vget_args vargs = {0};

	*vpp = NULL;
	pip = VTOI(pvp);
	ump = ITOUMP(pip);
	fs = ump->um_fs;
    cred = vfs_context_ucred(ctx);

	UFS_LOCK(ump);
	reclaimed = 0;
retry:
	if (fs->fs_cstotal.cs_nifree == 0)
		goto noinodes;

	if ((mode & IFMT) == IFDIR)
		ipref = ffs_dirpref(pip);
	else
		ipref = pip->i_number;
	if (ipref >= fs->fs_ncg * fs->fs_ipg)
		ipref = 0;
	cg = ino_to_cg(fs, ipref);
	/*
	 * Track number of dirs created one after another
	 * in a same cg without intervening by files.
	 */
	if ((mode & IFMT) == IFDIR) {
		if (fs->fs_contigdirs[cg] < 255)
			fs->fs_contigdirs[cg]++;
	} else {
		if (fs->fs_contigdirs[cg] > 0)
			fs->fs_contigdirs[cg]--;
	}
	ino = (ino_t)ffs_hashalloc(pip, cg, ipref, mode, 0,
					(allocfcn_t *)ffs_nodealloccg);
	if (ino == 0)
		goto noinodes;
	/*
	 * Get rid of the cached old vnode, force allocation of a new vnode
	 * for this inode. If this fails, release the allocated ino and
	 * return the error.
	 */
    // FIXME: pass mode to vget
    vargs.flags = FFSV_FORCEINSMQ | FFSV_REPLACE;
    vargs.dvp = pvp;
	if ((error = VFS_VGET(vnode_mount(pvp), ino, &vargs, vpp, ctx)) != 0) {
		ffs_vfree(pvp, ino, mode);
		return (error);
	}
	/*
	 * We got an inode, so check mode and panic if it is already allocated.
	 */
	ip = VTOI(*vpp);
	if (ip->i_mode) {
		log_debug("mode = 0%o, inum = %llu, fs = %s\n",
		    ip->i_mode, (uintmax_t)ip->i_number, fs->fs_fsmnt);
		panic("ffs_valloc: dup alloc");
	}
	if (DIP(ip, i_blocks) && (fs->fs_flags & FS_UNCLEAN) == 0) {  /* XXX */
		log_debug("free inode %s/%lu had %ld blocks\n",
		    fs->fs_fsmnt, (u_long)ino, (long)DIP(ip, i_blocks));
		DIP_SET(ip, i_blocks, 0);
	}
	ip->i_flags = 0;
	DIP_SET(ip, i_flags, 0);
	/*
	 * Set up a new generation number for this inode.
	 */
	while (ip->i_gen == 0 || ++ip->i_gen == 0)
		ip->i_gen = random();
	DIP_SET(ip, i_gen, (int)ip->i_gen);
	if (fs->fs_magic == FS_UFS2_MAGIC) {
		getnanotime(&ts);
		ip->i_din2->di_birthtime = ts.tv_sec;
		ip->i_din2->di_birthnsec = (int)ts.tv_nsec;
	}
	ip->i_flag = 0;
    
	return (0);
noinodes:
	if (reclaimed == 0) {
		reclaimed = 1;
		softdep_request_cleanup(fs, pvp, cred, FLUSH_INODES_WAIT);
		goto retry;
	}
	if (ffs_fsfail_cleanup_locked(ump, 0)) {
		UFS_UNLOCK(ump);
		return (ENXIO);
	}
	if (ppsratecheck(&ump->um_last_fullmsg, &ump->um_secs_fullmsg, 1)) {
		UFS_UNLOCK(ump);
		ffs_fserr(fs, pip->i_number, "out of inodes");
		log_debug("\n%s: create/symlink failed, no inodes free\n",
		    fs->fs_fsmnt);
	} else {
		UFS_UNLOCK(ump);
	}
	return (ENOSPC);
}

/*
 * Find a cylinder group to place a directory.
 *
 * The policy implemented by this algorithm is to allocate a
 * directory inode in the same cylinder group as its parent
 * directory, but also to reserve space for its files inodes
 * and data. Restrict the number of directories which may be
 * allocated one after another in the same cylinder group
 * without intervening allocation of files.
 *
 * If we allocate a first level directory then force allocation
 * in another cylinder group.
 */
static ino_t
ffs_dirpref(struct inode *pip)
{
	struct fs *fs;
	int cg, prefcg, dirsize, cgsize;
	u_int avgifree, avgbfree, avgndir, curdirsize;
	u_int minifree, minbfree, maxndir;
	u_int mincg, minndir;
	u_int maxcontigdirs;

	lck_mtx_assert(UFS_MTX(ITOUMP(pip)), LCK_MTX_ASSERT_OWNED);
	fs = ITOFS(pip);

	avgifree = (unsigned) fs->fs_cstotal.cs_nifree / fs->fs_ncg;
	avgbfree = (unsigned) fs->fs_cstotal.cs_nbfree / fs->fs_ncg;
	avgndir =  (unsigned) fs->fs_cstotal.cs_ndir / fs->fs_ncg;

	/*
	 * Force allocation in another cg if creating a first level dir.
	 */
	ASSERT_VNOP_LOCKED(ITOV(pip), "ffs_dirpref");
	if (vnode_isvroot(ITOV(pip))) {
		prefcg = random() % fs->fs_ncg;
		mincg = prefcg;
		minndir = fs->fs_ipg;
		for (cg = prefcg; cg < fs->fs_ncg; cg++)
			if (fs->fs_cs(fs, cg).cs_ndir < minndir &&
			    fs->fs_cs(fs, cg).cs_nifree >= avgifree &&
			    fs->fs_cs(fs, cg).cs_nbfree >= avgbfree) {
				mincg = cg;
				minndir = fs->fs_cs(fs, cg).cs_ndir;
			}
		for (cg = 0; cg < prefcg; cg++)
			if (fs->fs_cs(fs, cg).cs_ndir < minndir &&
			    fs->fs_cs(fs, cg).cs_nifree >= avgifree &&
			    fs->fs_cs(fs, cg).cs_nbfree >= avgbfree) {
				mincg = cg;
				minndir = fs->fs_cs(fs, cg).cs_ndir;
			}
		return ((ino_t)(fs->fs_ipg * mincg));
	}

	/*
	 * Count various limits which used for
	 * optimal allocation of a directory inode.
	 */
	maxndir = min(avgndir + fs->fs_ipg / 16, fs->fs_ipg);
	minifree = avgifree - avgifree / 4;
	if (minifree < 1)
		minifree = 1;
	minbfree = avgbfree - avgbfree / 4;
	if (minbfree < 1)
		minbfree = 1;
	cgsize = fs->fs_fsize * fs->fs_fpg;
	dirsize = fs->fs_avgfilesize * fs->fs_avgfpdir;
	curdirsize = avgndir ? (cgsize - avgbfree * fs->fs_bsize) / avgndir : 0;
	if (dirsize < curdirsize)
		dirsize = curdirsize;
	if (dirsize <= 0)
		maxcontigdirs = 0;		/* dirsize overflowed */
	else
		maxcontigdirs = min((avgbfree * fs->fs_bsize) / dirsize, 255);
	if (fs->fs_avgfpdir > 0)
		maxcontigdirs = min(maxcontigdirs,
				    fs->fs_ipg / fs->fs_avgfpdir);
	if (maxcontigdirs == 0)
		maxcontigdirs = 1;

	/*
	 * Limit number of dirs in one cg and reserve space for 
	 * regular files, but only if we have no deficit in
	 * inodes or space.
	 *
	 * We are trying to find a suitable cylinder group nearby
	 * our preferred cylinder group to place a new directory.
	 * We scan from our preferred cylinder group forward looking
	 * for a cylinder group that meets our criterion. If we get
	 * to the final cylinder group and do not find anything,
	 * we start scanning forwards from the beginning of the
	 * filesystem. While it might seem sensible to start scanning
	 * backwards or even to alternate looking forward and backward,
	 * this approach fails badly when the filesystem is nearly full.
	 * Specifically, we first search all the areas that have no space
	 * and finally try the one preceding that. We repeat this on
	 * every request and in the case of the final block end up
	 * searching the entire filesystem. By jumping to the front
	 * of the filesystem, our future forward searches always look
	 * in new cylinder groups so finds every possible block after
	 * one pass over the filesystem.
	 */
	prefcg = ino_to_cg(fs, pip->i_number);
	for (cg = prefcg; cg < fs->fs_ncg; cg++)
		if (fs->fs_cs(fs, cg).cs_ndir < maxndir &&
		    fs->fs_cs(fs, cg).cs_nifree >= minifree &&
		    fs->fs_cs(fs, cg).cs_nbfree >= minbfree) {
			if (fs->fs_contigdirs[cg] < maxcontigdirs)
				return ((ino_t)(fs->fs_ipg * cg));
		}
	for (cg = 0; cg < prefcg; cg++)
		if (fs->fs_cs(fs, cg).cs_ndir < maxndir &&
		    fs->fs_cs(fs, cg).cs_nifree >= minifree &&
		    fs->fs_cs(fs, cg).cs_nbfree >= minbfree) {
			if (fs->fs_contigdirs[cg] < maxcontigdirs)
				return ((ino_t)(fs->fs_ipg * cg));
		}
	/*
	 * This is a backstop when we have deficit in space.
	 */
	for (cg = prefcg; cg < fs->fs_ncg; cg++)
		if (fs->fs_cs(fs, cg).cs_nifree >= avgifree)
			return ((ino_t)(fs->fs_ipg * cg));
	for (cg = 0; cg < prefcg; cg++)
		if (fs->fs_cs(fs, cg).cs_nifree >= avgifree)
			break;
	return ((ino_t)(fs->fs_ipg * cg));
}

/*
 * Select the desired position for the next block in a file.  The file is
 * logically divided into sections. The first section is composed of the
 * direct blocks and the next fs_maxbpg blocks. Each additional section
 * contains fs_maxbpg blocks.
 *
 * If no blocks have been allocated in the first section, the policy is to
 * request a block in the same cylinder group as the inode that describes
 * the file. The first indirect is allocated immediately following the last
 * direct block and the data blocks for the first indirect immediately
 * follow it.
 *
 * If no blocks have been allocated in any other section, the indirect 
 * block(s) are allocated in the same cylinder group as its inode in an
 * area reserved immediately following the inode blocks. The policy for
 * the data blocks is to place them in a cylinder group with a greater than
 * average number of free blocks. An appropriate cylinder group is found
 * by using a rotor that sweeps the cylinder groups. When a new group of
 * blocks is needed, the sweep begins in the cylinder group following the
 * cylinder group from which the previous allocation was made. The sweep
 * continues until a cylinder group with greater than the average number
 * of free blocks is found. If the allocation is for the first block in an
 * indirect block or the previous block is a hole, then the information on
 * the previous allocation is unavailable; here a best guess is made based
 * on the logical block number being allocated.
 *
 * If a section is already partially allocated, the policy is to
 * allocate blocks contiguously within the section if possible.
 */
ufs2_daddr_t
ffs_blkpref_ufs1(struct inode *ip, ufs_lbn_t lbn, int indx, ufs1_daddr_t *bap)
{
	struct fs *fs;
	u_int cg, inocg;
	u_int avgbfree, startcg;
	ufs2_daddr_t pref, prevbn;

	ASSERT(indx <= 0 || bap != NULL, ("need non-NULL bap"));
	lck_mtx_assert(UFS_MTX(ITOUMP(ip)), LCK_MTX_ASSERT_OWNED);
	fs = ITOFS(ip);
	/*
	 * Allocation of indirect blocks is indicated by passing negative
	 * values in indx: -1 for single indirect, -2 for double indirect,
	 * -3 for triple indirect. As noted below, we attempt to allocate
	 * the first indirect inline with the file data. For all later
	 * indirect blocks, the data is often allocated in other cylinder
	 * groups. However to speed random file access and to speed up
	 * fsck, the filesystem reserves the first fs_metaspace blocks
	 * (typically half of fs_minfree) of the data area of each cylinder
	 * group to hold these later indirect blocks.
	 */
	inocg = ino_to_cg(fs, ip->i_number);
	if (indx < 0) {
		/*
		 * Our preference for indirect blocks is the zone at the
		 * beginning of the inode's cylinder group data area that
		 * we try to reserve for indirect blocks.
		 */
		pref = cgmeta(fs, inocg);
		/*
		 * If we are allocating the first indirect block, try to
		 * place it immediately following the last direct block.
		 */
		if (indx == -1 && lbn < UFS_NDADDR + NINDIR(fs) &&
		    ip->i_din1->di_db[UFS_NDADDR - 1] != 0)
			pref = ip->i_din1->di_db[UFS_NDADDR - 1] + fs->fs_frag;
		return (pref);
	}
	/*
	 * If we are allocating the first data block in the first indirect
	 * block and the indirect has been allocated in the data block area,
	 * try to place it immediately following the indirect block.
	 */
	if (lbn == UFS_NDADDR) {
		pref = ip->i_din1->di_ib[0];
		if (pref != 0 && pref >= cgdata(fs, inocg) &&
		    pref < cgbase(fs, inocg + 1))
			return (pref + fs->fs_frag);
	}
	/*
	 * If we are at the beginning of a file, or we have already allocated
	 * the maximum number of blocks per cylinder group, or we do not
	 * have a block allocated immediately preceding us, then we need
	 * to decide where to start allocating new blocks.
	 */
	if (indx ==  0) {
		prevbn = 0;
	} else {
		prevbn = bap[indx - 1];
		if (UFS_CHECK_BLKNO(ITOVFS(ip), ip->i_number, prevbn, fs->fs_bsize, 0) != 0)
			prevbn = 0;
	}
	if (indx % fs->fs_maxbpg == 0 || prevbn == 0) {
		/*
		 * If we are allocating a directory data block, we want
		 * to place it in the metadata area.
		 */
		if ((ip->i_mode & IFMT) == IFDIR)
			return (cgmeta(fs, inocg));
		/*
		 * Until we fill all the direct and all the first indirect's
		 * blocks, we try to allocate in the data area of the inode's
		 * cylinder group.
		 */
		if (lbn < UFS_NDADDR + NINDIR(fs))
			return (cgdata(fs, inocg));
		/*
		 * Find a cylinder with greater than average number of
		 * unused data blocks.
		 */
		if (indx == 0 || prevbn == 0)
			startcg = (int) (inocg + lbn / fs->fs_maxbpg);
		else
			startcg = (int) dtog(fs, prevbn) + 1;
		startcg %= fs->fs_ncg;
		avgbfree = (int) fs->fs_cstotal.cs_nbfree / fs->fs_ncg;
		for (cg = startcg; cg < fs->fs_ncg; cg++)
			if (fs->fs_cs(fs, cg).cs_nbfree >= avgbfree) {
				fs->fs_cgrotor = cg;
				return (cgdata(fs, cg));
			}
		for (cg = 0; cg <= startcg; cg++)
			if (fs->fs_cs(fs, cg).cs_nbfree >= avgbfree) {
				fs->fs_cgrotor = cg;
				return (cgdata(fs, cg));
			}
		return (0);
	}
	/*
	 * Otherwise, we just always try to lay things out contiguously.
	 */
	return (prevbn + fs->fs_frag);
}

/*
 * Same as above, but for UFS2
 */
ufs2_daddr_t
ffs_blkpref_ufs2(struct inode *ip, ufs_lbn_t lbn, int indx, ufs2_daddr_t *bap)
{
	struct fs *fs;
	u_int cg, inocg;
	u_int avgbfree, startcg;
	ufs2_daddr_t pref, prevbn;

	ASSERT(indx <= 0 || bap != NULL, ("need non-NULL bap"));
	lck_mtx_assert(UFS_MTX(ITOUMP(ip)), LCK_MTX_ASSERT_OWNED);
	fs = ITOFS(ip);
	/*
	 * Allocation of indirect blocks is indicated by passing negative
	 * values in indx: -1 for single indirect, -2 for double indirect,
	 * -3 for triple indirect. As noted below, we attempt to allocate
	 * the first indirect inline with the file data. For all later
	 * indirect blocks, the data is often allocated in other cylinder
	 * groups. However to speed random file access and to speed up
	 * fsck, the filesystem reserves the first fs_metaspace blocks
	 * (typically half of fs_minfree) of the data area of each cylinder
	 * group to hold these later indirect blocks.
	 */
	inocg = ino_to_cg(fs, ip->i_number);
	if (indx < 0) {
		/*
		 * Our preference for indirect blocks is the zone at the
		 * beginning of the inode's cylinder group data area that
		 * we try to reserve for indirect blocks.
		 */
		pref = cgmeta(fs, inocg);
		/*
		 * If we are allocating the first indirect block, try to
		 * place it immediately following the last direct block.
		 */
		if (indx == -1 && lbn < UFS_NDADDR + NINDIR(fs) &&
		    ip->i_din2->di_db[UFS_NDADDR - 1] != 0)
			pref = ip->i_din2->di_db[UFS_NDADDR - 1] + fs->fs_frag;
		return (pref);
	}
	/*
	 * If we are allocating the first data block in the first indirect
	 * block and the indirect has been allocated in the data block area,
	 * try to place it immediately following the indirect block.
	 */
	if (lbn == UFS_NDADDR) {
		pref = ip->i_din2->di_ib[0];
		if (pref != 0 && pref >= cgdata(fs, inocg) &&
		    pref < cgbase(fs, inocg + 1))
			return (pref + fs->fs_frag);
	}
	/*
	 * If we are at the beginning of a file, or we have already allocated
	 * the maximum number of blocks per cylinder group, or we do not
	 * have a block allocated immediately preceding us, then we need
	 * to decide where to start allocating new blocks.
	 */
	if (indx ==  0) {
		prevbn = 0;
	} else {
		prevbn = bap[indx - 1];
		if (UFS_CHECK_BLKNO(ITOVFS(ip), ip->i_number, prevbn, fs->fs_bsize, true) != 0)
			prevbn = 0;
	}
	if (indx % fs->fs_maxbpg == 0 || prevbn == 0) {
		/*
		 * If we are allocating a directory data block, we want
		 * to place it in the metadata area.
		 */
		if ((ip->i_mode & IFMT) == IFDIR)
			return (cgmeta(fs, inocg));
		/*
		 * Until we fill all the direct and all the first indirect's
		 * blocks, we try to allocate in the data area of the inode's
		 * cylinder group.
		 */
		if (lbn < UFS_NDADDR + NINDIR(fs))
			return (cgdata(fs, inocg));
		/*
		 * Find a cylinder with greater than average number of
		 * unused data blocks.
		 */
		if (indx == 0 || prevbn == 0)
			startcg = (int) (inocg + lbn / fs->fs_maxbpg);
		else
			startcg = (int) dtog(fs, prevbn) + 1;
		startcg %= fs->fs_ncg;
		avgbfree = (int) fs->fs_cstotal.cs_nbfree / fs->fs_ncg;
		for (cg = startcg; cg < fs->fs_ncg; cg++)
			if (fs->fs_cs(fs, cg).cs_nbfree >= avgbfree) {
				fs->fs_cgrotor = cg;
				return (cgdata(fs, cg));
			}
		for (cg = 0; cg <= startcg; cg++)
			if (fs->fs_cs(fs, cg).cs_nbfree >= avgbfree) {
				fs->fs_cgrotor = cg;
				return (cgdata(fs, cg));
			}
		return (0);
	}
	/*
	 * Otherwise, we just always try to lay things out contiguously.
	 */
	return (prevbn + fs->fs_frag);
}

/*
 * Implement the cylinder overflow algorithm.
 *
 * The policy implemented by this algorithm is:
 *   1) allocate the block in its requested cylinder group.
 *   2) quadradically rehash on the cylinder group number.
 *   3) brute force search for a free block.
 *
 * Must be called with the UFS lock held.  Will release the lock on success
 * and return with it held on failure.
 */
/*VARARGS5*/
static ufs2_daddr_t
ffs_hashalloc(ip, cg, pref, size, rsize, allocator)
	struct inode *ip;
	u_int cg;
	ufs2_daddr_t pref;
	int size;	/* Search size for data blocks, mode for inodes */
	int rsize;	/* Real allocated size. */
	allocfcn_t *allocator;
{
	struct fs *fs;
	ufs2_daddr_t result;
	u_int i, icg = cg;

	lck_mtx_assert(UFS_MTX(ITOUMP(ip)), LCK_MTX_ASSERT_OWNED);
#ifdef INVARIANTS
	if (vnode_mount(ITOV(ip))->mnt_kern_flag & MNTK_SUSPENDED)
		panic("ffs_hashalloc: allocation on suspended filesystem");
#endif
	fs = ITOFS(ip);
	/*
	 * 1: preferred cylinder group
	 */
	result = (*allocator)(ip, cg, pref, size, rsize);
	if (result)
		return (result);
	/*
	 * 2: quadratic rehash
	 */
	for (i = 1; i < fs->fs_ncg; i *= 2) {
		cg += i;
		if (cg >= fs->fs_ncg)
			cg -= fs->fs_ncg;
		result = (*allocator)(ip, cg, 0, size, rsize);
		if (result)
			return (result);
	}
	/*
	 * 3: brute force search
	 * Note that we start at i == 2, since 0 was checked initially,
	 * and 1 is always checked in the quadratic rehash.
	 */
	cg = (icg + 2) % fs->fs_ncg;
	for (i = 2; i < fs->fs_ncg; i++) {
		result = (*allocator)(ip, cg, 0, size, rsize);
		if (result)
			return (result);
		cg++;
		if (cg == fs->fs_ncg)
			cg = 0;
	}
	return (0);
}

/*
 * Determine whether a fragment can be extended.
 *
 * Check to see if the necessary fragments are available, and
 * if they are, allocate them.
 */
static ufs2_daddr_t
ffs_fragextend(struct inode *ip, u_int cg, ufs2_daddr_t bprev, int osize, int nsize)
{
	struct fs *fs;
	struct cg *cgp;
	struct buf *bp;
	struct ufsmount *ump;
	int nffree;
	long bno;
	int frags, bbase;
	int i, error;
	u_int8_t *blksfree;

	ump = ITOUMP(ip);
	fs = ump->um_fs;
	if (fs->fs_cs(fs, cg).cs_nffree < numfrags(fs, nsize - osize))
		return (0);
	frags = numfrags(fs, nsize);
	bbase = fragnum(fs, bprev);
	if (bbase > fragnum(fs, (bprev + frags - 1))) {
		/* cannot extend across a block boundary */
		return (0);
	}
	UFS_UNLOCK(ump);
	if ((error = ffs_getcg(fs, ump->um_devvp, cg, 0, &bp, &cgp)) != 0)
		goto fail;
	bno = dtogd(fs, bprev);
	blksfree = cg_blksfree(cgp);
	for (i = numfrags(fs, osize); i < frags; i++)
		if (isclr(blksfree, bno + i))
			goto fail;
	/*
	 * the current fragment can be extended
	 * deduct the count on fragment being extended into
	 * increase the count on the remaining fragment (if any)
	 * allocate the extended piece
	 */
	for (i = frags; i < fs->fs_frag - bbase; i++)
		if (isclr(blksfree, bno + i))
			break;
	cgp->cg_frsum[i - numfrags(fs, osize)]--;
	if (i != frags)
		cgp->cg_frsum[i - frags]++;
	for (i = numfrags(fs, osize), nffree = 0; i < frags; i++) {
		clrbit(blksfree, bno + i);
		cgp->cg_cs.cs_nffree--;
		nffree++;
	}
	UFS_LOCK(ump);
	fs->fs_cstotal.cs_nffree -= nffree;
	fs->fs_cs(fs, cg).cs_nffree -= nffree;
	fs->fs_fmod = 1;
	ACTIVECLEAR(fs, cg);
	UFS_UNLOCK(ump);
	if (DOINGSOFTDEP(ITOV(ip)))
		softdep_setup_blkmapdep(bp, UFSTOVFS(ump), bprev,
		    frags, numfrags(fs, osize));
	buf_bdwrite(bp);
	return (bprev);

fail:
	buf_brelse(bp);
	UFS_LOCK(ump);
	return (0);

}

/*
 * Determine whether a block can be allocated.
 *
 * Check to see if a block of the appropriate size is available,
 * and if it is, allocate it.
 */
static ufs2_daddr_t
ffs_alloccg(struct inode *ip, u_int cg, ufs2_daddr_t bpref, int size, int rsize)
{
	struct fs *fs;
	struct cg *cgp;
	struct buf *bp;
	struct ufsmount *ump;
	ufs1_daddr_t bno;
	ufs2_daddr_t blkno;
	int i, allocsiz, error, frags;
	u_int8_t *blksfree;

	ump = ITOUMP(ip);
	fs = ump->um_fs;
	if (fs->fs_cs(fs, cg).cs_nbfree == 0 && size == fs->fs_bsize)
		return (0);
	UFS_UNLOCK(ump);
	if ((error = ffs_getcg(fs, ump->um_devvp, cg, 0, &bp, &cgp)) != 0 ||
	   (cgp->cg_cs.cs_nbfree == 0 && size == fs->fs_bsize))
		goto fail;
	if (size == fs->fs_bsize) {
		UFS_LOCK(ump);
		blkno = ffs_alloccgblk(ip, bp, bpref, rsize);
		ACTIVECLEAR(fs, cg);
		UFS_UNLOCK(ump);
		buf_bdwrite(bp);
		return (blkno);
	}
	/*
	 * check to see if any fragments are already available
	 * allocsiz is the size which will be allocated, hacking
	 * it down to a smaller size if necessary
	 */
	blksfree = cg_blksfree(cgp);
	frags = numfrags(fs, size);
	for (allocsiz = frags; allocsiz < fs->fs_frag; allocsiz++)
		if (cgp->cg_frsum[allocsiz] != 0)
			break;
	if (allocsiz == fs->fs_frag) {
		/*
		 * no fragments were available, so a block will be
		 * allocated, and hacked up
		 */
		if (cgp->cg_cs.cs_nbfree == 0)
			goto fail;
		UFS_LOCK(ump);
		blkno = ffs_alloccgblk(ip, bp, bpref, rsize);
		ACTIVECLEAR(fs, cg);
		UFS_UNLOCK(ump);
		buf_bdwrite(bp);
		return (blkno);
	}
	ASSERT(size == rsize,
	    ("ffs_alloccg: size(%d) != rsize(%d)", size, rsize));
	bno = ffs_mapsearch(fs, cgp, bpref, allocsiz);
	if (bno < 0)
		goto fail;
	for (i = 0; i < frags; i++)
		clrbit(blksfree, bno + i);
	cgp->cg_cs.cs_nffree -= frags;
	cgp->cg_frsum[allocsiz]--;
	if (frags != allocsiz)
		cgp->cg_frsum[allocsiz - frags]++;
	UFS_LOCK(ump);
	fs->fs_cstotal.cs_nffree -= frags;
	fs->fs_cs(fs, cg).cs_nffree -= frags;
	fs->fs_fmod = 1;
	blkno = cgbase(fs, cg) + bno;
	ACTIVECLEAR(fs, cg);
	UFS_UNLOCK(ump);
	if (DOINGSOFTDEP(ITOV(ip)))
		softdep_setup_blkmapdep(bp, UFSTOVFS(ump), blkno, frags, 0);
	buf_bdwrite(bp);
	return (blkno);

fail:
	buf_brelse(bp);
	UFS_LOCK(ump);
	return (0);
}

/*
 * Allocate a block in a cylinder group.
 *
 * This algorithm implements the following policy:
 *   1) allocate the requested block.
 *   2) allocate a rotationally optimal block in the same cylinder.
 *   3) allocate the next available block on the block rotor for the
 *      specified cylinder group.
 * Note that this routine only allocates fs_bsize blocks; these
 * blocks may be fragmented by the routine that allocates them.
 */
static ufs2_daddr_t
ffs_alloccgblk(struct inode *ip, struct buf *bp, ufs2_daddr_t bpref, int size)
{
	struct fs *fs;
	struct cg *cgp;
	struct ufsmount *ump;
	ufs1_daddr_t bno;
	ufs2_daddr_t blkno;
	u_int8_t *blksfree;
	int i, cgbpref;

	ump = ITOUMP(ip);
	fs = ump->um_fs;
	lck_mtx_assert(UFS_MTX(ump), LCK_MTX_ASSERT_OWNED);
	cgp = (struct cg *)buf_dataptr(bp);
	blksfree = cg_blksfree(cgp);
	if (bpref == 0) {
		bpref = cgbase(fs, cgp->cg_cgx) + cgp->cg_rotor + fs->fs_frag;
	} else if ((cgbpref = (int) dtog(fs, bpref)) != cgp->cg_cgx) {
		/* map bpref to correct zone in this cg */
		if (bpref < cgdata(fs, cgbpref))
			bpref = cgmeta(fs, cgp->cg_cgx);
		else
			bpref = cgdata(fs, cgp->cg_cgx);
	}
	/*
	 * if the requested block is available, use it
	 */
	bno = dtogd(fs, blknum(fs, bpref));
	if (ffs_isblock(fs, blksfree, fragstoblks(fs, bno)))
		goto gotit;
	/*
	 * Take the next available block in this cylinder group.
	 */
	bno = ffs_mapsearch(fs, cgp, bpref, (int)fs->fs_frag);
	if (bno < 0)
		return (0);
	/* Update cg_rotor only if allocated from the data zone */
	if (bno >= dtogd(fs, cgdata(fs, cgp->cg_cgx)))
		cgp->cg_rotor = bno;
gotit:
	blkno = fragstoblks(fs, bno);
	ffs_clrblock(fs, blksfree, (int)blkno);
	ffs_clusteracct(fs, cgp, (int)blkno, -1);
	cgp->cg_cs.cs_nbfree--;
	fs->fs_cstotal.cs_nbfree--;
	fs->fs_cs(fs, cgp->cg_cgx).cs_nbfree--;
	fs->fs_fmod = 1;
	blkno = cgbase(fs, cgp->cg_cgx) + bno;
	/*
	 * If the caller didn't want the whole block free the frags here.
	 */
	size = numfrags(fs, size);
	if (size != fs->fs_frag) {
		bno = dtogd(fs, blkno);
		for (i = size; i < fs->fs_frag; i++)
			setbit(blksfree, bno + i);
		i = fs->fs_frag - size;
		cgp->cg_cs.cs_nffree += i;
		fs->fs_cstotal.cs_nffree += i;
		fs->fs_cs(fs, cgp->cg_cgx).cs_nffree += i;
		fs->fs_fmod = 1;
		cgp->cg_frsum[i]++;
	}
	/* XXX Fixme. */
	UFS_UNLOCK(ump);
	if (DOINGSOFTDEP(ITOV(ip)))
		softdep_setup_blkmapdep(bp, UFSTOVFS(ump), blkno, size, 0);
	UFS_LOCK(ump);
	return (blkno);
}

/*
 * Determine whether a cluster can be allocated.
 *
 * We do not currently check for optimal rotational layout if there
 * are multiple choices in the same cylinder group. Instead we just
 * take the first one that we find following bpref.
 */
static ufs2_daddr_t __unused
ffs_clusteralloc(struct inode *ip, u_int cg, ufs2_daddr_t bpref, int len)
{
	struct fs *fs;
	struct cg *cgp;
	struct buf *bp;
	struct ufsmount *ump;
	int i, run, bit, map, got, error;
	ufs2_daddr_t bno;
	u_char *mapp;
	int32_t *lp;
	u_int8_t *blksfree;

	ump = ITOUMP(ip);
	fs = ump->um_fs;
	if (fs->fs_maxcluster[cg] < len)
		return (0);
	UFS_UNLOCK(ump);
	if ((error = ffs_getcg(fs, ump->um_devvp, cg, 0, &bp, &cgp)) != 0) {
		UFS_LOCK(ump);
		return (0);
	}
	/*
	 * Check to see if a cluster of the needed size (or bigger) is
	 * available in this cylinder group.
	 */
	lp = &cg_clustersum(cgp)[len];
	for (i = len; i <= fs->fs_contigsumsize; i++)
		if (*lp++ > 0)
			break;
	if (i > fs->fs_contigsumsize) {
		/*
		 * This is the first time looking for a cluster in this
		 * cylinder group. Update the cluster summary information
		 * to reflect the true maximum sized cluster so that
		 * future cluster allocation requests can avoid reading
		 * the cylinder group map only to find no clusters.
		 */
		lp = &cg_clustersum(cgp)[len - 1];
		for (i = len - 1; i > 0; i--)
			if (*lp-- > 0)
				break;
		UFS_LOCK(ump);
		fs->fs_maxcluster[cg] = i;
		buf_brelse(bp);
		return (0);
	}
	/*
	 * Search the cluster map to find a big enough cluster.
	 * We take the first one that we find, even if it is larger
	 * than we need as we prefer to get one close to the previous
	 * block allocation. We do not search before the current
	 * preference point as we do not want to allocate a block
	 * that is allocated before the previous one (as we will
	 * then have to wait for another pass of the elevator
	 * algorithm before it will be read). We prefer to fail and
	 * be recalled to try an allocation in the next cylinder group.
	 */
	if (dtog(fs, bpref) != cg)
		bpref = cgdata(fs, cg);
	else
		bpref = blknum(fs, bpref);
	bpref = fragstoblks(fs, dtogd(fs, bpref));
	mapp = &cg_clustersfree(cgp)[bpref / NBBY];
	map = *mapp++;
	bit = 1 << (bpref % NBBY);
	for (run = 0, got = (int) bpref; got < cgp->cg_nclusterblks; got++) {
		if ((map & bit) == 0) {
			run = 0;
		} else {
			run++;
			if (run == len)
				break;
		}
		if ((got & (NBBY - 1)) != (NBBY - 1)) {
			bit <<= 1;
		} else {
			map = *mapp++;
			bit = 1;
		}
	}
	if (got >= cgp->cg_nclusterblks) {
		UFS_LOCK(ump);
		buf_brelse(bp);
		return (0);
	}
	/*
	 * Allocate the cluster that we have found.
	 */
	blksfree = cg_blksfree(cgp);
	for (i = 1; i <= len; i++)
		if (!ffs_isblock(fs, blksfree, got - run + i))
			panic("ffs_clusteralloc: map mismatch");
	bno = cgbase(fs, cg) + blkstofrags(fs, got - run + 1);
	if (dtog(fs, bno) != cg)
		panic("ffs_clusteralloc: allocated out of group");
	len = blkstofrags(fs, len);
	UFS_LOCK(ump);
	for (i = 0; i < len; i += fs->fs_frag)
		if (ffs_alloccgblk(ip, bp, bno + i, fs->fs_bsize) != bno + i)
			panic("ffs_clusteralloc: lost block");
	ACTIVECLEAR(fs, cg);
	UFS_UNLOCK(ump);
	buf_bdwrite(bp);
	return (bno);
}

static inline struct buf *
getinobuf(struct inode *ip, u_int cg, u_int32_t cginoblk, int gbflags)
{
	struct fs *fs;

	fs = ITOFS(ip);
	return (getblk(ITODEVVP(ip), fsbtodb(fs, ino_to_fsba(fs,
	    cg * fs->fs_ipg + cginoblk)), (int)fs->fs_bsize, 0, 0,
	    gbflags));
}

/*
 * Synchronous inode initialization is needed only when barrier writes do not
 * work as advertised, and will impose a heavy cost on file creation in a newly
 * created filesystem.
 */
static int doasyncinodeinit = 1;
SYSCTL_INT(_vfs_ffs, OID_AUTO, doasyncinodeinit, CTLFLAG_RW, &doasyncinodeinit, 0,
    "Perform inode block initialization using asynchronous writes");

/*
 * Determine whether an inode can be allocated.
 *
 * Check to see if an inode is available, and if it is,
 * allocate it using the following policy:
 *   1) allocate the requested inode.
 *   2) allocate the next available inode after the requested
 *      inode in the specified cylinder group.
 */
static ufs2_daddr_t
ffs_nodealloccg(struct inode *ip, u_int cg, ufs2_daddr_t ipref, int mode, int unused)
{
	struct fs *fs;
	struct cg *cgp;
	struct buf *bp, *ibp;
	struct ufsmount *ump;
	u_int8_t *inosused, *loc;
	struct ufs2_dinode *dp2;
	int error, start, len, i;
	u_int32_t old_initediblk;

	ump = ITOUMP(ip);
	fs = ump->um_fs;
check_nifree:
	if (fs->fs_cs(fs, cg).cs_nifree == 0)
		return (0);
	UFS_UNLOCK(ump);
	if ((error = ffs_getcg(fs, ump->um_devvp, cg, 0, &bp, &cgp)) != 0) {
		UFS_LOCK(ump);
		return (0);
	}
restart:
	if (cgp->cg_cs.cs_nifree == 0) {
		buf_brelse(bp);
		UFS_LOCK(ump);
		return (0);
	}
	inosused = cg_inosused(cgp);
	if (ipref) {
		ipref %= fs->fs_ipg;
		if (isclr(inosused, ipref))
			goto gotit;
	}
	start = cgp->cg_irotor / NBBY;
	len = howmany(fs->fs_ipg - cgp->cg_irotor, NBBY);
	loc = memcchr(&inosused[start], 0xff, len);
	if (loc == NULL) {
		len = start + 1;
		start = 0;
		loc = memcchr(&inosused[start], 0xff, len);
		if (loc == NULL) {
			log_debug("cg = %d, irotor = %ld, fs = %s\n",
			    cg, (long)cgp->cg_irotor, fs->fs_fsmnt);
			panic("ffs_nodealloccg: map corrupted");
			/* NOTREACHED */
		}
	}
	ipref = (loc - inosused) * NBBY + ffs(~*loc) - 1;
gotit:
	/*
	 * Check to see if we need to initialize more inodes.
	 */
	if (fs->fs_magic == FS_UFS2_MAGIC &&
	    ipref + INOPB(fs) > cgp->cg_initediblk &&
	    cgp->cg_initediblk < cgp->cg_niblk) {
		old_initediblk = cgp->cg_initediblk;

		/*
		 * Free the cylinder group lock before writing the
		 * initialized inode block.  Entering the
		 * babarrierwrite() with the cylinder group lock
		 * causes lock order violation between the lock and
		 * snaplk.
		 *
		 * Another thread can decide to initialize the same
		 * inode block, but whichever thread first gets the
		 * cylinder group lock after writing the newly
		 * allocated inode block will update it and the other
		 * will realize that it has lost and leave the
		 * cylinder group unchanged.
		 */
		ibp = getinobuf(ip, cg, old_initediblk, GB_UFS_LOCK_NOWAIT);
		buf_brelse(bp);
		if (ibp == NULL) {
			/*
			 * The inode block buffer is already owned by
			 * another thread, which must initialize it.
			 * Wait on the buffer to allow another thread
			 * to finish the updates, with dropped cg
			 * buffer lock, then retry.
			 */
			ibp = getinobuf(ip, cg, old_initediblk, 0);
			buf_brelse(ibp);
			UFS_LOCK(ump);
			goto check_nifree;
		}
		bzero((void*)buf_dataptr(ibp), (int)fs->fs_bsize);
		dp2 = (struct ufs2_dinode *)(buf_dataptr(ibp));
		for (i = 0; i < INOPB(fs); i++) {
			while (dp2->di_gen == 0)
				dp2->di_gen = random();
			dp2++;
		}

		/*
		 * Rather than adding a soft updates dependency to ensure
		 * that the new inode block is written before it is claimed
		 * by the cylinder group map, we just do a barrier write
		 * here. The barrier write will ensure that the inode block
		 * gets written before the updated cylinder group map can be
		 * written. The barrier write should only slow down bulk
		 * loading of newly created filesystems.
		 */
		if (doasyncinodeinit){
            dk_synchronize_t syncop = { .options  = DK_SYNCHRONIZE_OPTION_BARRIER };
            buf_bawrite(ibp);
            VNOP_IOCTL(ump->um_devvp, DKIOCSYNCHRONIZE, (caddr_t)&syncop, FWRITE, vfs_context_current());
        } else
			bwrite(ibp);

		/*
		 * After the inode block is written, try to update the
		 * cg initediblk pointer.  If another thread beat us
		 * to it, then leave it unchanged as the other thread
		 * has already set it correctly.
		 */
		error = ffs_getcg(fs, ump->um_devvp, cg, 0, &bp, &cgp);
		UFS_LOCK(ump);
		ACTIVECLEAR(fs, cg);
		UFS_UNLOCK(ump);
		if (error != 0)
			return (error);
		if (cgp->cg_initediblk == old_initediblk)
			cgp->cg_initediblk += INOPB(fs);
		goto restart;
	}
	cgp->cg_irotor = (int) ipref;
	UFS_LOCK(ump);
	ACTIVECLEAR(fs, cg);
	setbit(inosused, ipref);
	cgp->cg_cs.cs_nifree--;
	fs->fs_cstotal.cs_nifree--;
	fs->fs_cs(fs, cg).cs_nifree--;
	fs->fs_fmod = 1;
	if ((mode & IFMT) == IFDIR) {
		cgp->cg_cs.cs_ndir++;
		fs->fs_cstotal.cs_ndir++;
		fs->fs_cs(fs, cg).cs_ndir++;
	}
	UFS_UNLOCK(ump);
	if (DOINGSOFTDEP(ITOV(ip)))
		softdep_setup_inomapdep(bp, ip, (int)(cg * fs->fs_ipg + ipref), mode);
	buf_bdwrite(bp);
	return ((ino_t)(cg * fs->fs_ipg + ipref));
}

/*
 * Free a block or fragment.
 *
 * The specified block or fragment is placed back in the
 * free map. If a fragment is deallocated, a possible
 * block reassembly is checked.
 */
static void
ffs_blkfree_cg(struct ufsmount *ump, struct fs *fs, struct vnode *devvp, ufs2_daddr_t bno, long size, ino_t inum, struct workhead *dephd)
{
	struct mount *mp;
	struct cg *cgp;
	struct buf *bp;
	daddr_t dbn;
	ufs1_daddr_t fragno, cgbno;
	int i, blk, frags, bbase, error;
	u_int cg;
	u_int8_t *blksfree;
	dev_t dev;

	cg = (int) dtog(fs, bno);
	if (vnode_vtype(devvp) == VREG) {
		/* devvp is a snapshot */
		MPASS(VFSTOUFS(vnode_mount(devvp)) == ump);
		dev = vnode_specrdev(ump->um_devvp);
	} else if (vnode_vtype(devvp) == VCHR) {
		/* devvp is a normal disk device */
		dev =vnode_specrdev(devvp);
		ASSERT_VNOP_LOCKED(devvp, "ffs_blkfree_cg");
	} else
		return;
#ifdef INVARIANTS
	if ((u_int)size > fs->fs_bsize || fragoff(fs, size) != 0 ||
	    fragnum(fs, bno) + numfrags(fs, size) > fs->fs_frag) {
		log_debug("dev=%s, bno = %lld, bsize = %ld, size = %ld, fs = %s\n",
		    devtoname(devvp), (intmax_t)bno, (long)fs->fs_bsize,
		    size, fs->fs_fsmnt);
		panic("ffs_blkfree_cg: bad size");
	}
#endif
	if ((u_int)bno >= fs->fs_size) {
		log_debug("bad block %lld, ino %lu\n", (intmax_t)bno,
		    (u_long)inum);
		ffs_fserr(fs, inum, "bad block");
		return;
	}
	if ((error = ffs_getcg(fs, devvp, cg, BX_CVTENXIO, &bp, &cgp)) != 0) {
		if (!ffs_fsfail_cleanup(ump, error) ||
		    !MOUNTEDSOFTDEP(UFSTOVFS(ump)) || vnode_vtype(devvp) != VCHR)
			return;
		if (vnode_vtype(devvp) == VREG)
			dbn = (int)fragstoblks(fs, cgtod(fs, cg));
		else
			dbn = fsbtodb(fs, cgtod(fs, cg));
		bp = getblk(devvp, dbn, fs->fs_cgsize, 0, 0, BLK_META);
		ASSERT(bp != NULL, ("getblkx failed"));
		softdep_setup_blkfree(UFSTOVFS(ump), bp, bno,
		    (int)numfrags(fs, size), dephd);
		buf_setflags(bp, B_NOCACHE);
		buf_bawrite(bp);
		return;
	}
	cgbno = dtogd(fs, bno);
	blksfree = cg_blksfree(cgp);
	UFS_LOCK(ump);
	if (size == fs->fs_bsize) {
		fragno = fragstoblks(fs, cgbno);
		if (!ffs_isfreeblock(fs, blksfree, fragno)) {
			if (vnode_vtype(devvp) == VREG) {
				UFS_UNLOCK(ump);
				/* devvp is a snapshot */
				buf_brelse(bp);
				return;
			}
			log_debug("dev = %s, block = %lld, fs = %s\n",
			    devtoname(devvp), (intmax_t)bno, fs->fs_fsmnt);
			panic("ffs_blkfree_cg: freeing free block");
		}
		ffs_setblock(fs, blksfree, fragno);
		ffs_clusteracct(fs, cgp, fragno, 1);
		cgp->cg_cs.cs_nbfree++;
		fs->fs_cstotal.cs_nbfree++;
		fs->fs_cs(fs, cg).cs_nbfree++;
	} else {
		bbase = cgbno - fragnum(fs, cgbno);
		/*
		 * decrement the counts associated with the old frags
		 */
		blk = blkmap(fs, blksfree, bbase);
		ffs_fragacct(fs, blk, (int*)cgp->cg_frsum, -1);
		/*
		 * deallocate the fragment
		 */
		frags = (int)numfrags(fs, size);
		for (i = 0; i < frags; i++) {
			if (isset(blksfree, cgbno + i)) {
				log_debug("dev = %s, block = %lld, fs = %s\n",
				    devtoname(devvp), (intmax_t)(bno + i),
				    fs->fs_fsmnt);
				panic("ffs_blkfree_cg: freeing free frag");
			}
			setbit(blksfree, cgbno + i);
		}
		cgp->cg_cs.cs_nffree += i;
		fs->fs_cstotal.cs_nffree += i;
		fs->fs_cs(fs, cg).cs_nffree += i;
		/*
		 * add back in counts associated with the new frags
		 */
		blk = blkmap(fs, blksfree, bbase);
		ffs_fragacct(fs, blk, (int*)cgp->cg_frsum, 1);
		/*
		 * if a complete block has been reassembled, account for it
		 */
		fragno = fragstoblks(fs, bbase);
		if (ffs_isblock(fs, blksfree, fragno)) {
			cgp->cg_cs.cs_nffree -= fs->fs_frag;
			fs->fs_cstotal.cs_nffree -= fs->fs_frag;
			fs->fs_cs(fs, cg).cs_nffree -= fs->fs_frag;
			ffs_clusteracct(fs, cgp, fragno, 1);
			cgp->cg_cs.cs_nbfree++;
			fs->fs_cstotal.cs_nbfree++;
			fs->fs_cs(fs, cg).cs_nbfree++;
		}
	}
	fs->fs_fmod = 1;
	ACTIVECLEAR(fs, cg);
	UFS_UNLOCK(ump);
	mp = UFSTOVFS(ump);
	if (MOUNTEDSOFTDEP(mp) && vnode_vtype(devvp) == VCHR)
		softdep_setup_blkfree(UFSTOVFS(ump), bp, bno,
		    (int)numfrags(fs, size), dephd);
	buf_bdwrite(bp);
}

/*
 * Structures and routines associated with trim management.
 *
 * The following requests are passed to trim_lookup to indicate
 * the actions that should be taken.
 */
#define	NEW	1	/* if found, error else allocate and hash it */
#define	OLD	2	/* if not found, error, else return it */
#define	REPLACE	3	/* if not found, error else unhash and reallocate it */
#define	DONE	4	/* if not found, error else unhash and return it */
#define	SINGLE	5	/* don't look up, just allocate it and don't hash it */

MALLOC_DEFINE(M_TRIM, "ufs_trim", "UFS trim structures");

#define	TRIMLIST_HASH(ump, key) \
	(&(ump)->um_trimhash[(key) & (ump)->um_trimlisthashsize])

/*
 * These structures describe each of the block free requests aggregated
 * together to make up a trim request.
 */
struct trim_blkreq {
	TAILQ_ENTRY(trim_blkreq) blkreqlist;
	ufs2_daddr_t bno;
	long size;
	struct workhead *pdephd;
	struct workhead dephd;
};

/*
 * Description of a trim request.
 */
struct ffs_blkfree_trim_params {
	TAILQ_HEAD(, trim_blkreq) blklist;
	LIST_ENTRY(ffs_blkfree_trim_params) hashlist;
	struct task task;
	struct ufsmount *ump;
	struct vnode *devvp;
	ino_t inum;
	ufs2_daddr_t bno;
	long size;
	long key;
};

static void	ffs_blkfree_trim_completed(struct ffs_blkfree_trim_params *tp);
static void	ffs_blkfree_trim_task(void *ctx);
static struct	ffs_blkfree_trim_params *trim_lookup(struct ufsmount *,
		    struct vnode *, ufs2_daddr_t, long, ino_t, u_long, int);
static void	ffs_blkfree_sendtrim(struct ffs_blkfree_trim_params *);

/*
 * Called on trim completion to start a task to free the associated block(s).
 */
static void
ffs_blkfree_trim_completed(struct ffs_blkfree_trim_params *tp)
{
	TASK_INIT(&tp->task, 0, ffs_blkfree_trim_task, tp);
	taskqueue_enqueue(tp->ump->um_trim_tq, &tp->task);
}

/*
 * Trim completion task that free associated block(s).
 */
static void
ffs_blkfree_trim_task(void *ctx)
{
	struct ffs_blkfree_trim_params *tp;
	struct trim_blkreq *blkelm;
	struct ufsmount *ump;

	tp = ctx;
	ump = tp->ump;
	while ((blkelm = TAILQ_FIRST(&tp->blklist)) != NULL) {
		ffs_blkfree_cg(ump, ump->um_fs, tp->devvp, blkelm->bno,
                       blkelm->size, tp->inum, blkelm->pdephd);
		TAILQ_REMOVE(&tp->blklist, blkelm, blkreqlist);
        free(blkelm, M_TRIM);
	}
	vn_finished_secondary_write(UFSTOVFS(ump));
	UFS_LOCK(ump);
	ump->um_trim_inflight -= 1;
	ump->um_trim_inflight_blks -= numfrags(ump->um_fs, tp->size);
	UFS_UNLOCK(ump);
    free(tp, M_TRIM);
}

/*
 * Lookup a trim request by inode number.
 * Allocate if requested (NEW, REPLACE, SINGLE).
 */
static struct ffs_blkfree_trim_params *
trim_lookup(struct ufsmount *ump, struct vnode *devvp, ufs2_daddr_t bno, long size, ino_t inum, u_long key, int alloctype)
{
	struct trimlist_hashhead *tphashhead;
	struct ffs_blkfree_trim_params *tp, *ntp;

    tphashhead = NULL; // silence warnings
    tp = NULL; // silence warnings
	ntp = malloc(sizeof(struct ffs_blkfree_trim_params), M_TRIM, M_WAITOK);
	if (alloctype != SINGLE) {
		ASSERT(key >= FIRST_VALID_KEY, ("trim_lookup: invalid key"));
		UFS_LOCK(ump);
		tphashhead = TRIMLIST_HASH(ump, key);
		LIST_FOREACH(tp, tphashhead, hashlist)
			if (key == tp->key)
				break;
	}
	switch (alloctype) {
	case NEW:
		ASSERT(tp == NULL, ("trim_lookup: found trim"));
		break;
	case OLD:
		ASSERT(tp != NULL,  ("trim_lookup: missing call to ffs_blkrelease_start()"));
		UFS_UNLOCK(ump);
        free(ntp, M_TRIM);
		return (tp);
	case REPLACE:
		ASSERT(tp != NULL, ("trim_lookup: missing REPLACE trim"));
		LIST_REMOVE(tp, hashlist);
		/* tp will be freed by caller */
		break;
	case DONE:
		ASSERT(tp != NULL, ("trim_lookup: missing DONE trim"));
		LIST_REMOVE(tp, hashlist);
		UFS_UNLOCK(ump);
		free(ntp, M_TRIM);
		return (tp);
	}
	TAILQ_INIT(&ntp->blklist);
	ntp->ump = ump;
	ntp->devvp = devvp;
	ntp->bno = bno;
	ntp->size = size;
	ntp->inum = inum;
	ntp->key = key;
	if (alloctype != SINGLE) {
		LIST_INSERT_HEAD(tphashhead, ntp, hashlist);
		UFS_UNLOCK(ump);
	}
	return (ntp);
}

/*
 * Dispatch a trim request.
 */
static void
ffs_blkfree_sendtrim(struct ffs_blkfree_trim_params *tp)
{
	struct ufsmount *ump;
    dk_extent_t extent;
    dk_unmap_t info;

	/*
	 * Postpone the set of the free bit in the cg bitmap until the
	 * BIO_DELETE is completed.  Otherwise, due to disk queue
	 * reordering, TRIM might be issued after we reuse the block
	 * and write some new data into it.
	 */
    
    ump = tp->ump;
    extent.offset = fsbtodb(ump->um_fs, tp->bno);
    extent.length = tp->size;
    info.extents = &extent;
    info.extentsCount = 1;
    
	UFS_LOCK(ump);
	ump->um_trim_total += 1;
	ump->um_trim_inflight += 1;
	ump->um_trim_inflight_blks += numfrags(ump->um_fs, tp->size);
	ump->um_trim_total_blks += numfrags(ump->um_fs, tp->size);
	UFS_UNLOCK(ump);
    VNOP_IOCTL(tp->devvp, DKIOCUNMAP, (caddr_t)&info, FWRITE, vfs_context_current());
    ffs_blkfree_trim_completed(tp);
}

/*
 * Allocate a new key to use to identify a range of blocks.
 */
u_long
ffs_blkrelease_start(struct ufsmount *ump, struct vnode *devvp, ino_t inum)
{
	static atomic_ulong masterkey;
	u_long key;

	if (((ump->um_flags & UM_CANDELETE) == 0) || dotrimcons == 0)
		return (SINGLETON_KEY);
	do {
		key = atomic_fetch_add(&masterkey, 1);
	} while (key < FIRST_VALID_KEY);
	(void) trim_lookup(ump, devvp, 0, 0, inum, key, NEW);
	return (key);
}

/*
 * Deallocate a key that has been used to identify a range of blocks.
 */
void
ffs_blkrelease_finish(struct ufsmount *ump, u_long key)
{
	struct ffs_blkfree_trim_params *tp;

	if (((ump->um_flags & UM_CANDELETE) == 0) || dotrimcons == 0)
		return;
	/*
	 * If the vfs.ffs.dotrimcons sysctl option is enabled while
	 * a file deletion is active, specifically after a call
	 * to ffs_blkrelease_start() but before the call to
	 * ffs_blkrelease_finish(), ffs_blkrelease_start() will
	 * have handed out SINGLETON_KEY rather than starting a
	 * collection sequence. Thus if we get a SINGLETON_KEY
	 * passed to ffs_blkrelease_finish(), we just return rather
	 * than trying to finish the nonexistent sequence.
	 */
	if (key == SINGLETON_KEY) {
#ifdef INVARIANTS
		log_debug("%s: vfs.ffs.dotrimcons enabled on active filesystem\n",
		    vfs_statfs(ump->um_mountp)->f_mntonname);
#endif
		return;
	}
	/*
	 * We are done with sending blocks using this key. Look up the key
	 * using the DONE alloctype (in tp) to request that it be unhashed
	 * as we will not be adding to it. If the key has never been used,
	 * tp->size will be zero, so we can just free tp. Otherwise the call
	 * to ffs_blkfree_sendtrim(tp) causes the block range described by
	 * tp to be issued (and then tp to be freed).
	 */
	tp = trim_lookup(ump, NULL, 0, 0, 0, key, DONE);
	if (tp->size == 0)
		free(tp, M_TRIM);
	else
		ffs_blkfree_sendtrim(tp);
}

/*
 * Setup to free a block or fragment.
 *
 * Check for snapshots that might want to claim the block.
 * If trims are requested, prepare a trim request. Attempt to
 * aggregate consecutive blocks into a single trim request.
 */
void
ffs_blkfree( struct ufsmount *ump, struct fs *fs, struct vnode *devvp, ufs2_daddr_t bno,
            long size, ino_t inum, enum vtype vtype, struct workhead *dephd, u_long key)
{
	struct ffs_blkfree_trim_params *tp, *ntp;
	struct trim_blkreq *blkelm;

	/*
	 * Check to see if a snapshot wants to claim the block.
	 * Check that devvp is a normal disk device, not a snapshot,
	 * it has a snapshot(s) associated with it, and one of the
	 * snapshots wants to claim the block.
	 */
	if (vnode_vtype(devvp) == VCHR &&
//	    (devvp->v_vflag & VV_COPYONWRITE) &&
	    ffs_snapblkfree(fs, devvp, bno, size, inum, vtype, dephd)) {
		return;
	}
	/*
	 * Nothing to delay if TRIM is not required for this block or TRIM
	 * is disabled or the operation is performed on a snapshot.
	 */
	if (key == NOTRIM_KEY || ((ump->um_flags & UM_CANDELETE) == 0) ||
	    vnode_vtype(devvp) == VREG) {
		ffs_blkfree_cg(ump, fs, devvp, bno, size, inum, dephd);
		return;
	}
	blkelm = malloc(sizeof(struct trim_blkreq), M_TRIM, M_WAITOK);
	blkelm->bno = bno;
	blkelm->size = size;
	if (dephd == NULL) {
		blkelm->pdephd = NULL;
	} else {
		LIST_INIT(&blkelm->dephd);
		LIST_SWAP(dephd, &blkelm->dephd, worklist, wk_list);
		blkelm->pdephd = &blkelm->dephd;
	}
	if (key == SINGLETON_KEY) {
		/*
		 * Just a single non-contiguous piece. Use the SINGLE
		 * alloctype to return a trim request that will not be
		 * hashed for future lookup.
		 */
		tp = trim_lookup(ump, devvp, bno, size, inum, key, SINGLE);
		TAILQ_INSERT_HEAD(&tp->blklist, blkelm, blkreqlist);
		ffs_blkfree_sendtrim(tp);
		return;
	}
	/*
	 * The callers of this function are not tracking whether or not
	 * the blocks are contiguous. They are just saying that they
	 * are freeing a set of blocks. It is this code that determines
	 * the pieces of that range that are actually contiguous.
	 *
	 * Calling ffs_blkrelease_start() will have created an entry
	 * that we will use.
	 */
	tp = trim_lookup(ump, devvp, bno, size, inum, key, OLD);
	if (tp->size == 0) {
		/*
		 * First block of a potential range, set block and size
		 * for the trim block.
		 */
		tp->bno = bno;
		tp->size = size;
		TAILQ_INSERT_HEAD(&tp->blklist, blkelm, blkreqlist);
		return;
	}
	/*
	 * If this block is a continuation of the range (either
	 * follows at the end or preceeds in the front) then we
	 * add it to the front or back of the list and return.
	 *
	 * If it is not a continuation of the trim that we were
	 * building, using the REPLACE alloctype, we request that
	 * the old trim request (still in tp) be unhashed and a
	 * new range started (in ntp). The ffs_blkfree_sendtrim(tp)
	 * call causes the block range described by tp to be issued
	 * (and then tp to be freed).
	 */
	if (bno + numfrags(fs, size) == tp->bno) {
		TAILQ_INSERT_HEAD(&tp->blklist, blkelm, blkreqlist);
		tp->bno = bno;
		tp->size += size;
		return;
	} else if (bno == tp->bno + numfrags(fs, tp->size)) {
		TAILQ_INSERT_TAIL(&tp->blklist, blkelm, blkreqlist);
		tp->size += size;
		return;
	}
	ntp = trim_lookup(ump, devvp, bno, size, inum, key, REPLACE);
	TAILQ_INSERT_HEAD(&ntp->blklist, blkelm, blkreqlist);
	ffs_blkfree_sendtrim(tp);
}

#ifdef INVARIANTS
/*
 * Verify allocation of a block or fragment. Returns true if block or
 * fragment is allocated, false if it is free.
 */
static int
ffs_checkblk(struct inode *ip, ufs2_daddr_t bno, long size)
{
	struct fs *fs;
	struct cg *cgp;
	struct buf *bp;
	ufs1_daddr_t cgbno;
	int i, error, frags, free;
	u_int8_t *blksfree;

	fs = ITOFS(ip);
	if ((u_int)size > fs->fs_bsize || fragoff(fs, size) != 0) {
		log_debug("bsize = %ld, size = %ld, fs = %s\n",
		    (long)fs->fs_bsize, size, fs->fs_fsmnt);
		panic("ffs_checkblk: bad size");
	}
	if ((u_int)bno >= fs->fs_size)
		panic("ffs_checkblk: bad block %lld", (intmax_t)bno);
	error = ffs_getcg(fs, ITODEVVP(ip), dtog(fs, bno), 0, &bp, &cgp);
	if (error)
		panic("ffs_checkblk: cylinder group read failed");
	blksfree = cg_blksfree(cgp);
	cgbno = dtogd(fs, bno);
	if (size == fs->fs_bsize) {
		free = ffs_isblock(fs, blksfree, fragstoblks(fs, cgbno));
	} else {
		frags = numfrags(fs, size);
		for (free = 0, i = 0; i < frags; i++)
			if (isset(blksfree, cgbno + i))
				free++;
		if (free != 0 && free != frags)
			panic("ffs_checkblk: partially free fragment");
	}
	buf_brelse(bp);
	return (!free);
}
#endif /* INVARIANTS */

/*
 * Free an inode.
 */
int
ffs_vfree(struct vnode *pvp, ino_t ino, int mode)
{
	struct ufsmount *ump;

	if (DOINGSOFTDEP(pvp)) {
		softdep_freefile(pvp, ino, mode);
		return (0);
	}
	ump = VFSTOUFS(vnode_mount(pvp));
	return (ffs_freefile(ump, ump->um_fs, ump->um_devvp, ino, mode, NULL));
}

/*
 * Do the actual free operation.
 * The specified inode is placed back in the free map.
 */
int
ffs_freefile(struct ufsmount *ump, struct fs *fs, struct vnode *devvp, ino_t ino, int mode, struct workhead *wkhd)
{
	struct cg *cgp;
	struct buf *bp;
	daddr_t dbn;
	int error;
	u_int cg;
	u_int8_t *inosused;
	dev_t dev;
	ino_t cgino;

	cg = ino_to_cg(fs, ino);
	if (vnode_vtype(devvp) == VREG) {
		/* devvp is a snapshot */
		MPASS(vfs_fsprivate(vnode_mount(devvp)) == ump);
        dev = vnode_specrdev(ump->um_devvp);
	} else if (vnode_vtype(devvp) == VCHR) {
		/* devvp is a normal disk device */
        dev = vnode_specrdev(devvp);
	} else {
		bp = NULL;
		return (0);
	}
	if (ino >= fs->fs_ipg * fs->fs_ncg)
		panic("ffs_freefile: range: dev = %s, ino = %llu, fs = %s",
		    devtoname(devvp), (uintmax_t)ino, fs->fs_fsmnt);
	if ((error = ffs_getcg(fs, devvp, cg, BX_CVTENXIO, &bp, &cgp)) != 0) {
		if (!ffs_fsfail_cleanup(ump, error) ||
		    !MOUNTEDSOFTDEP(UFSTOVFS(ump)) || vnode_vtype(devvp) != VCHR)
			return (error);
		if (vnode_vtype(devvp) == VREG)
			dbn = (int)fragstoblks(fs, cgtod(fs, cg));
		else
			dbn = fsbtodb(fs, cgtod(fs, cg));
		bp = getblk(devvp, dbn, fs->fs_cgsize, 0, 0, BLK_META);
		ASSERT(bp != NULL, ("getblkx failed"));
		softdep_setup_inofree(UFSTOVFS(ump), bp, ino, wkhd);
		buf_setflags(bp, B_NOCACHE);
		buf_bawrite(bp);
		return (error);
	}
	inosused = cg_inosused(cgp);
	cgino = ino % fs->fs_ipg;
	if (isclr(inosused, cgino)) {
		log_debug("dev = %s, ino = %llu, fs = %s\n", devtoname(devvp),
		    (uintmax_t)ino, fs->fs_fsmnt);
		if (fs->fs_ronly == 0)
			panic("ffs_freefile: freeing free inode");
	}
	clrbit(inosused, cgino);
	if (cgino < cgp->cg_irotor)
		cgp->cg_irotor = cgino;
	cgp->cg_cs.cs_nifree++;
	UFS_LOCK(ump);
	fs->fs_cstotal.cs_nifree++;
	fs->fs_cs(fs, cg).cs_nifree++;
	if ((mode & IFMT) == IFDIR) {
		cgp->cg_cs.cs_ndir--;
		fs->fs_cstotal.cs_ndir--;
		fs->fs_cs(fs, cg).cs_ndir--;
	}
	fs->fs_fmod = 1;
	ACTIVECLEAR(fs, cg);
	UFS_UNLOCK(ump);
	if (MOUNTEDSOFTDEP(UFSTOVFS(ump)) && vnode_vtype(devvp) == VCHR)
		softdep_setup_inofree(UFSTOVFS(ump), bp, ino, wkhd);
	buf_bdwrite(bp);
	return (0);
}

/*
 * Check to see if a file is free.
 * Used to check for allocated files in snapshots.
 */
int
ffs_checkfreefile(struct fs *fs, struct vnode *devvp, ino_t ino)
{
	struct cg *cgp;
	struct buf *bp;
	int ret, error;
	u_int cg;
	u_int8_t *inosused;

	cg = ino_to_cg(fs, ino);
	if ((vnode_vtype(devvp) != VREG) && (vnode_vtype(devvp) != VCHR))
		return (1);
	if (ino >= fs->fs_ipg * fs->fs_ncg)
		return (1);
	if ((error = ffs_getcg(fs, devvp, cg, 0, &bp, &cgp)) != 0)
		return (1);
	inosused = cg_inosused(cgp);
	ino %= fs->fs_ipg;
	ret = isclr(inosused, ino);
	buf_brelse(bp);
	return (ret);
}

/*
 * Find a block of the specified size in the specified cylinder group.
 *
 * It is a panic if a request is made to find a block if none are
 * available.
 */
static ufs1_daddr_t
ffs_mapsearch(struct fs *fs, struct cg *cgp, ufs2_daddr_t bpref, int allocsiz)
{
	ufs1_daddr_t bno;
	int start, len, loc, i;
	int blk, field, subfield, pos;
	u_int8_t *blksfree;

	/*
	 * find the fragment by searching through the free block
	 * map for an appropriate bit pattern
	 */
	if (bpref)
		start = dtogd(fs, bpref) / NBBY;
	else
		start = cgp->cg_frotor / NBBY;
	blksfree = cg_blksfree(cgp);
	len = howmany(fs->fs_fpg, NBBY) - start;
	loc = scanc((u_int)len, (u_char *)&blksfree[start],
		fragtbl[fs->fs_frag],
		(u_char)(1 << (allocsiz - 1 + (fs->fs_frag % NBBY))));
	if (loc == 0) {
		len = start + 1;
		start = 0;
		loc = scanc((u_int)len, (u_char *)&blksfree[0],
			fragtbl[fs->fs_frag],
			(u_char)(1 << (allocsiz - 1 + (fs->fs_frag % NBBY))));
		if (loc == 0) {
			log_debug("start = %d, len = %d, fs = %s\n",
			    start, len, fs->fs_fsmnt);
			panic("ffs_alloccg: map corrupted");
			/* NOTREACHED */
		}
	}
	bno = (start + len - loc) * NBBY;
	cgp->cg_frotor = bno;
	/*
	 * found the byte in the map
	 * sift through the bits to find the selected frag
	 */
	for (i = bno + NBBY; bno < i; bno += fs->fs_frag) {
		blk = blkmap(fs, blksfree, bno);
		blk <<= 1;
		field = around[allocsiz];
		subfield = inside[allocsiz];
		for (pos = 0; pos <= fs->fs_frag - allocsiz; pos++) {
			if ((blk & field) == subfield)
				return (bno + pos);
			field <<= 1;
			subfield <<= 1;
		}
	}
	log_debug("bno = %lu, fs = %s\n", (u_long)bno, fs->fs_fsmnt);
	panic("ffs_alloccg: block not in map");
	return (-1);
}

static const struct vfsstatfs *
ffs_getmntstat(struct vnode *devvp)
{

	if (vnode_vtype(devvp) == VCHR)
		return (vfs_statfs(vnode_mountedhere(devvp)));
	return (ffs_getmntstat(VFSTOUFS(vnode_mount(devvp))->um_devvp));
}

/*
 * Fetch and verify a cylinder group.
 */
int
ffs_getcg(struct fs *fs, struct vnode *devvp, u_int cg, int flags, struct buf **bpp, struct cg **cgpp)
{
	struct buf *bp;
    struct bufpriv *bpriv;
	struct cg *cgp;
	const struct vfsstatfs *sfs;
	daddr_t blkno;
	int error;

	*bpp = NULL;
	*cgpp = NULL;
	if ((fs->fs_metackhash & CK_CYLGRP) != 0)
		flags |= GB_CKHASH;
	if (vnode_vtype(devvp) == VREG)
		blkno = (int)fragstoblks(fs, cgtod(fs, cg));
	else
		blkno = fsbtodb(fs, cgtod(fs, cg));
	error = breadn_flags(devvp, blkno, blkno, (int)fs->fs_cgsize, NULL,
                         NULL, 0, NOCRED, flags, ffs_ckhash_cg, &bp);
	if (error != 0)
		return (error);
    bpriv = buf_fsprivate(bp);
	cgp = (struct cg *)buf_dataptr(bp);
	if ((fs->fs_metackhash & CK_CYLGRP) != 0 &&
	    buf_checked_hash(bp) &&
	    cgp->cg_ckhash != bpriv->b_ckhash) {
		sfs = ffs_getmntstat(devvp);
		log_debug("UFS %s%s (%s) cylinder checksum failed: cg %u, cgp: "
               "0x%x != bp: 0x%llx\n",
		    vnode_vtype(devvp) == VCHR ? "" : "snapshot of ",
		    sfs->f_mntfromname, sfs->f_mntonname,
		    cg, cgp->cg_ckhash, bpriv->b_ckhash);
		bpriv->b_cflags &= ~BC_CKHASH;
		buf_markinvalid(bp);
        buf_setflags(bp, B_NOCACHE);
		buf_brelse(bp);
		return (EIO);
	}
	if (!cg_chkmagic(cgp) || cgp->cg_cgx != cg) {
		sfs = ffs_getmntstat(devvp);
		log_debug("UFS %s%s (%s)",
		    vnode_vtype(devvp) == VCHR ? "" : "snapshot of ",
		    sfs->f_mntfromname, sfs->f_mntonname);
		if (!cg_chkmagic(cgp))
			log_debug(" cg %u: bad magic number 0x%x should be 0x%x\n", cg, cgp->cg_magic, CG_MAGIC);
		else
			log_debug(": wrong cylinder group cg %u != cgx %u\n", cg, cgp->cg_cgx);
		bpriv->b_cflags &= ~BC_CKHASH;
		buf_markinvalid(bp);
        buf_setflags(bp, B_NOCACHE);
		buf_brelse(bp);
		return (EIO);
	}
	bpriv->b_cflags &= ~BC_CKHASH;
	buf_setxflags(bp, BX_BKGRDWRITE);
	/*
	 * If we are using check hashes on the cylinder group then we want
	 * to limit changing the cylinder group time to when we are actually
	 * going to write it to disk so that its check hash remains correct
	 * in memory. If the CK_CYLGRP flag is set the time is updated in
	 * ffs_bufwrite() as the buffer is queued for writing. Otherwise we
	 * update the time here as we have done historically.
	 */
	if ((fs->fs_metackhash & CK_CYLGRP) != 0)
		buf_setxflags(bp, BX_CYLGRP);
	else
		cgp->cg_old_time = cgp->cg_time = (int)time_seconds();
	*bpp = bp;
	*cgpp = cgp;
	return (0);
}

static void
ffs_ckhash_cg(struct buf *bp)
{
	uint32_t ckhash;
	struct cg *cgp;
    struct bufpriv *bpriv;

    bpriv = buf_fsprivate(bp);
	cgp = (struct cg *)buf_dataptr(bp);
	ckhash = cgp->cg_ckhash;
	cgp->cg_ckhash = 0;
	bpriv->b_ckhash = calculate_crc32c(~0L, (uint8_t*)buf_dataptr(bp), buf_count(bp));
	cgp->cg_ckhash = ckhash;
}

/*
 * Fserr prints the name of a filesystem with an error diagnostic.
 *
 * The form of the error message is:
 *	fs: error message
 */
void
ffs_fserr(struct fs *fs, ino_t inum, char *cp)
{
	struct ucred *cred = vfs_context_ucred(vfs_context_current());
    pid_t pid = proc_pid(current_proc());
    char pcomm[1024] = {0};
    
    proc_name(pid, &pcomm[0], sizeof(pcomm));
    
	log(LOG_ERR, "pid %d (%s), uid %d inumber %llu on %s: %s\n",
	    pid, pcomm, cred->cr_posix.cr_uid, inum,
	    fs->fs_fsmnt, cp);
}

/*
 * This function provides the capability for the fsck program to
 * update an active filesystem. Fourteen operations are provided:
 *
 * adjrefcnt(inode, amt) - adjusts the reference count on the
 *	specified inode by the specified amount. Under normal
 *	operation the count should always go down. Decrementing
 *	the count to zero will cause the inode to be freed.
 * adjblkcnt(inode, amt) - adjust the number of blocks used by the
 *	inode by the specified amount.
 * setsize(inode, size) - set the size of the inode to the
 *	specified size.
 * adjndir, adjbfree, adjifree, adjffree, adjnumclusters(amt) -
 *	adjust the superblock summary.
 * freedirs(inode, count) - directory inodes [inode..inode + count - 1]
 *	are marked as free. Inodes should never have to be marked
 *	as in use.
 * freefiles(inode, count) - file inodes [inode..inode + count - 1]
 *	are marked as free. Inodes should never have to be marked
 *	as in use.
 * freeblks(blockno, size) - blocks [blockno..blockno + size - 1]
 *	are marked as free. Blocks should never have to be marked
 *	as in use.
 * setflags(flags, set/clear) - the fs_flags field has the specified
 *	flags set (second parameter +1) or cleared (second parameter -1).
 * setcwd(dirinode) - set the current directory to dirinode in the
 *	filesystem associated with the snapshot.
 * setdotdot(oldvalue, newvalue) - Verify that the inode number for ".."
 *	in the current directory is oldvalue then change it to newvalue.
 * unlink(nameptr, oldvalue) - Verify that the inode number associated
 *	with nameptr in the current directory is oldvalue then unlink it.
 */

static int sysctl_ffs_fsck SYSCTL_HANDLER_ARGS;

SYSCTL_PROC(_vfs_ffs, FFS_ADJ_REFCNT, adjrefcnt, CTLFLAG_WR | CTLTYPE_STRUCT, 0, 0, sysctl_ffs_fsck, "S,fsck",
    "Adjust Inode Reference Count");

static SYSCTL_NODE(_vfs_ffs, FFS_ADJ_BLKCNT, adjblkcnt, CTLFLAG_WR, sysctl_ffs_fsck,
    "Adjust Inode Used Blocks Count");

static SYSCTL_NODE(_vfs_ffs, FFS_SET_SIZE, setsize, CTLFLAG_WR, sysctl_ffs_fsck,
    "Set the inode size");

static SYSCTL_NODE(_vfs_ffs, FFS_ADJ_NDIR, adjndir, CTLFLAG_WR, sysctl_ffs_fsck,
    "Adjust number of directories");

static SYSCTL_NODE(_vfs_ffs, FFS_ADJ_NBFREE, adjnbfree,
    CTLFLAG_WR, sysctl_ffs_fsck,
    "Adjust number of free blocks");

static SYSCTL_NODE(_vfs_ffs, FFS_ADJ_NIFREE, adjnifree, CTLFLAG_WR, sysctl_ffs_fsck,
    "Adjust number of free inodes");

static SYSCTL_NODE(_vfs_ffs, FFS_ADJ_NFFREE, adjnffree, CTLFLAG_WR, sysctl_ffs_fsck,
    "Adjust number of free frags");

static SYSCTL_NODE(_vfs_ffs, FFS_ADJ_NUMCLUSTERS, adjnumclusters,CTLFLAG_WR, sysctl_ffs_fsck,
    "Adjust number of free clusters");

static SYSCTL_NODE(_vfs_ffs, FFS_DIR_FREE, freedirs, CTLFLAG_WR, sysctl_ffs_fsck,
    "Free Range of Directory Inodes");

static SYSCTL_NODE(_vfs_ffs, FFS_FILE_FREE, freefiles, CTLFLAG_WR, sysctl_ffs_fsck,
    "Free Range of File Inodes");

static SYSCTL_NODE(_vfs_ffs, FFS_BLK_FREE, freeblks, CTLFLAG_WR, sysctl_ffs_fsck,
    "Free Range of Blocks");

static SYSCTL_NODE(_vfs_ffs, FFS_SET_FLAGS, setflags, CTLFLAG_WR, sysctl_ffs_fsck,
    "Change Filesystem Flags");

static SYSCTL_NODE(_vfs_ffs, FFS_SET_CWD, setcwd, CTLFLAG_WR, sysctl_ffs_fsck,
    "Set Current Working Directory");

static SYSCTL_NODE(_vfs_ffs, FFS_SET_DOTDOT, setdotdot, CTLFLAG_WR, sysctl_ffs_fsck,
    "Change Value of .. Entry");

static SYSCTL_NODE(_vfs_ffs, FFS_UNLINK, unlink, CTLFLAG_WR, sysctl_ffs_fsck,
    "Unlink a Duplicate Name");

#ifdef DIAGNOSTIC
static int fsckcmds = 0;
SYSCTL_INT(_debug, OID_AUTO, ffs_fsckcmds, CTLFLAG_RW, &fsckcmds, 0,
	"print out fsck_ffs-based filesystem update commands");
#endif /* DIAGNOSTIC */

static int
sysctl_ffs_fsck SYSCTL_HANDLER_ARGS
{
    vfs_context_t context;
    proc_t proc;
	struct fsck_cmd cmd;
	struct ufsmount *ump;
	struct vnode *vp, *dvp, *fdvp;
	struct inode *ip, *dp;
	struct mount *mp;
	struct fs *fs;
	ufs2_daddr_t blkno;
	long blkcnt, blksize;
	u_long key;
    unsigned vid;
	int filetype, error;

    context = vfs_context_current();
    proc = current_proc();
    
	if (req->newlen > sizeof cmd)
		return (EBADRPC);
	if ((error = SYSCTL_IN(req, &cmd, sizeof cmd)) != 0)
		return (error);
	if (cmd.version != FFS_CMD_VERSION)
		return (ERPCMISMATCH);
    
	if ((error = file_vnode_withvid(cmd.handle, &vp, &vid)) != 0)
		return (error);
	if (vnode_vtype(vp) != VREG && vnode_vtype(vp) != VDIR) {
        file_drop(cmd.handle);
		return (EINVAL);
	}
	vn_start_write(vp, &mp, V_WAIT);
	if (mp == NULL || strncmp(vfs_statfs(mp)->f_fstypename, "ufsX", MFSNAMELEN)) {
		vn_finished_write(mp);
		file_drop(cmd.handle);
		return (EINVAL);
	}
	ump = VFSTOUFS(mp);
	if ((vfs_flags(mp) & MNT_RDONLY) && ump->um_fsckpid != proc_pid(proc)) {
		vn_finished_write(mp);
        file_drop(cmd.handle);
		return (EROFS);
	}
	fs = ump->um_fs;
	filetype = IFREG;

    switch (oidp->oid_number) {
        case FFS_SET_FLAGS:
#ifdef DIAGNOSTIC
            if (fsckcmds)
                log_debug("%s: %s flags\n", vfs_statfs(mp)->f_mntonname,
                       cmd.size > 0 ? "set" : "clear");
#endif /* DIAGNOSTIC */
            if (cmd.size > 0)
                fs->fs_flags |= (long)cmd.value;
            else
                fs->fs_flags &= ~(long)cmd.value;
            break;
            
        case FFS_ADJ_REFCNT:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust inode %lld link count by %lld\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value,
                       (intmax_t)cmd.size);
            }
#endif /* DIAGNOSTIC */
            if ((error = ffs_vget(mp, (ino_t)cmd.value, &vp, context)))
                break;
            ip = VTOI(vp);
            ip->i_nlink += cmd.size;
            DIP_SET(ip, i_nlink, ip->i_nlink);
            ip->i_effnlink += cmd.size;
            UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_MODIFIED);
            error = ffs_update(vp, 1);
            if (DOINGSOFTDEP(vp))
                softdep_change_linkcnt(ip);
            vnode_put(vp);
            break;
            
        case FFS_ADJ_BLKCNT:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust inode %lld block count by %lld\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value,
                       (intmax_t)cmd.size);
            }
#endif /* DIAGNOSTIC */
            if ((error = ffs_vget(mp, (ino_t)cmd.value, &vp, context)))
                break;
            ip = VTOI(vp);
            DIP_SET(ip, i_blocks, DIP(ip, i_blocks) + cmd.size);
            UFS_INODE_SET_FLAG(ip, IN_CHANGE | IN_MODIFIED);
            error = ffs_update(vp, 1);
            vnode_put(vp);
            break;
            
        case FFS_SET_SIZE:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: set inode %lld size to %lld\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value,
                       (intmax_t)cmd.size);
            }
#endif /* DIAGNOSTIC */
            if ((error = ffs_vget(mp, (ino_t)cmd.value, &vp, context)))
                break;
            ip = VTOI(vp);
            DIP_SET(ip, i_size, cmd.size);
            UFS_INODE_SET_FLAG(ip, IN_SIZEMOD | IN_CHANGE | IN_MODIFIED);
            error = ffs_update(vp, 1);
            vnode_put(vp);
            break;
            
        case FFS_DIR_FREE:
            filetype = IFDIR;
            /* fall through */
            
        case FFS_FILE_FREE:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                if (cmd.size == 1)
                    log_debug("%s: free %s inode %llu\n",
                           vfs_statfs(mp)->f_mntonname,
                           filetype == IFDIR ? "directory" : "file",
                           (uintmax_t)cmd.value);
                else
                    log_debug("%s: free %s inodes %llu-%llu\n",
                           vfs_statfs(mp)->f_mntonname,
                           filetype == IFDIR ? "directory" : "file",
                           (uintmax_t)cmd.value,
                           (uintmax_t)(cmd.value + cmd.size - 1));
            }
#endif /* DIAGNOSTIC */
            while (cmd.size > 0) {
                if ((error = ffs_freefile(ump, fs, ump->um_devvp,
                                          (ino_t)cmd.value, filetype, NULL)))
                    break;
                cmd.size -= 1;
                cmd.value += 1;
            }
            break;
            
        case FFS_BLK_FREE:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                if (cmd.size == 1)
                    log_debug("%s: free block %lld\n",
                           vfs_statfs(mp)->f_mntonname,
                           (intmax_t)cmd.value);
                else
                    log_debug("%s: free blocks %lld-%lld\n",
                           vfs_statfs(mp)->f_mntonname,
                           (intmax_t)cmd.value,
                           (intmax_t)cmd.value + cmd.size - 1);
            }
#endif /* DIAGNOSTIC */
            blkno = cmd.value;
            blkcnt = cmd.size;
            blksize = fs->fs_frag - (blkno % fs->fs_frag);
            key = ffs_blkrelease_start(ump, ump->um_devvp, UFS_ROOTINO);
            while (blkcnt > 0) {
                if (blkcnt < blksize)
                    blksize = blkcnt;
                ffs_blkfree(ump, fs, ump->um_devvp, blkno,
                            blksize * fs->fs_fsize, UFS_ROOTINO,
                            VDIR, NULL, key);
                blkno += blksize;
                blkcnt -= blksize;
                blksize = fs->fs_frag;
            }
            ffs_blkrelease_finish(ump, key);
            break;
            
            /*
             * Adjust superblock summaries.  fsck(8) is expected to
             * submit deltas when necessary.
             */
        case FFS_ADJ_NDIR:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust number of directories by %lld\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value);
            }
#endif /* DIAGNOSTIC */
            fs->fs_cstotal.cs_ndir += cmd.value;
            break;
            
        case FFS_ADJ_NBFREE:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust number of free blocks by %+jd\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value);
            }
#endif /* DIAGNOSTIC */
            fs->fs_cstotal.cs_nbfree += cmd.value;
            break;
            
        case FFS_ADJ_NIFREE:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust number of free inodes by %+jd\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value);
            }
#endif /* DIAGNOSTIC */
            fs->fs_cstotal.cs_nifree += cmd.value;
            break;
            
        case FFS_ADJ_NFFREE:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust number of free frags by %+jd\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value);
            }
#endif /* DIAGNOSTIC */
            fs->fs_cstotal.cs_nffree += cmd.value;
            break;
            
        case FFS_ADJ_NUMCLUSTERS:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: adjust number of free clusters by %+jd\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value);
            }
#endif /* DIAGNOSTIC */
            fs->fs_cstotal.cs_numclusters += cmd.value;
            break;
            
        case FFS_SET_CWD:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: set current directory to inode %lld\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value);
            }
#endif /* DIAGNOSTIC */
#if 0
            if ((error = ffs_vget(mp, (ino_t)cmd.value, &vp, context)))
                break;
            
            struct chdir_args { char *path; };
            extern int _chdir(proc_t p, struct chdir_args *uap, __unused int32_t *retval);
            
            char pathbuf[PATH_MAX] = {0};
            int pathlen;
            
            // get file path
            error = vn_getpath(vp, pathbuf, &pathlen);
            if (error) {
                vnode_put(vp);
                break;
            }
            struct chdir_args args = { .path = &pathbuf[0], };
            error = _chdir(current_proc(), &args, &pathlen);
#else
            error = ENOTSUP;
            break;
#endif
            
//            if ((error = change_dir(vp, td)) != 0) {
//                vnode_put(vp);
//                break;
//            }
//
//            pwd_chdir(td, vp);
            break;
            
        case FFS_SET_DOTDOT:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("%s: change .. in cwd from %lld to %lld\n",
                       vfs_statfs(mp)->f_mntonname, (intmax_t)cmd.value,
                       (intmax_t)cmd.size);
            }
#endif /* DIAGNOSTIC */
            /*
             * First we have to get and lock the parent directory
             * to which ".." points.
             */
            error = ffs_vget(mp, (ino_t)cmd.value, &fdvp, context);
            if (error)
                break;
            /*
             * Now we get and lock the child directory containing "..".
             */
            
            dvp = vfs_context_cwd(context);
            if ((error = vnode_get(dvp)) != 0) {
                vnode_put(fdvp);
                break;
            }
            dp = VTOI(dvp);
            SET_I_OFFSET(dp, 12);	/* XXX mastertemplate.dot_reclen */
            error = ufs_dirrewrite(dp, VTOI(fdvp), (ino_t)cmd.size, DT_DIR, 0);
            cache_purge(fdvp);
            cache_purge(dvp);
            vnode_put(dvp);
            vnode_put(fdvp);
            break;
            
        case FFS_UNLINK:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                char buf[32];
                
                if (copyinstr((char *)(intptr_t)cmd.value, buf,32,NULL))
                    strncpy(buf, "Name_too_long", 32);
                log_debug("%s: unlink %s (inode %lld)\n",
                       vfs_statfs(mp)->f_mntonname, buf, (intmax_t)cmd.size);
            }
#endif /* DIAGNOSTIC */
            /*
             * kern_funlinkat will do its own start/finish writes and
             * they do not nest, so drop ours here. Setting mp == NULL
             * indicates that vn_finished_write is not needed down below.
             */
            vn_finished_write(mp);
#if 0
            mp = NULL;
            extern int unlinkat(int fd, user_addr_t path, int flag);
            error = unlinkat(AT_FDCWD, (uintptr_t)cmd.value, VNODE_REMOVE_NO_AUDIT_PATH);
            break;
#else
            error = ENOTSUP;
            break;
#endif
        default:
#ifdef DIAGNOSTIC
            if (fsckcmds) {
                log_debug("Invalid request %d from fsck\n",
                       oidp->oid_number);
            }
#endif /* DIAGNOSTIC */
            error = EINVAL;
            break;
    }
    file_drop(cmd.handle);
	vn_finished_write(mp);
	return (error);
}
