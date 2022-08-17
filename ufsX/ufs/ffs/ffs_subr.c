/*-
 * SPDX-License-Identifier: BSD-3-Clause
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
 *	@(#)ffs_subr.c	8.5 (Berkeley) 3/21/95
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>

#ifndef _KERNEL

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/errno.h>
#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>

uint32_t calculate_crc32c(uint32_t, const void *, size_t);
uint32_t ffs_calc_sbhash(struct fs *);
struct malloc_type;
#define UFS_MALLOC(size, type, flags) malloc(size)
#define UFS_FREE(ptr, type) free(ptr)
#define log_debug(a...)
#define panic(a...)
#define trace_return(n) return (n)
#else /* _KERNEL */
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/ucred.h>

#include <mach/mach_time.h>
#include <libkern/libkern.h> // crc32
#include <freebsd/compat/compat.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ffs/fs.h>

#define UFS_MALLOC(size, type, flags)   malloc(size, type, flags)
#define UFS_FREE(ptr, type)             free(ptr, type)
#endif /* _KERNEL */

/*
 * Verify an inode check-hash.
 */
int
ffs_verify_dinode_ckhash(struct fs *fs, struct ufs2_dinode *dip)
{
	uint32_t ckhash, save_ckhash;

	/*
	 * Return success if unallocated or we are not doing inode check-hash.
	 */
	if (dip->di_mode == 0 || (fs->fs_metackhash & CK_INODE) == 0)
		trace_return (0);
	/*
	 * Exclude di_ckhash from the crc32 calculation, e.g., always use
	 * a check-hash value of zero when calculating the check-hash.
	 */
	save_ckhash = dip->di_ckhash;
	dip->di_ckhash = 0;
	ckhash = calculate_crc32c(~0L, (void *)dip, sizeof(*dip));
	dip->di_ckhash = save_ckhash;
	if (save_ckhash == ckhash)
		trace_return (0);
	trace_return (EINVAL);
}

/*
 * Update an inode check-hash.
 */
void
ffs_update_dinode_ckhash(struct fs *fs, struct ufs2_dinode *dip)
{

	if (dip->di_mode == 0 || (fs->fs_metackhash & CK_INODE) == 0)
		return;
	/*
	 * Exclude old di_ckhash from the crc32 calculation, e.g., always use
	 * a check-hash value of zero when calculating the new check-hash.
	 */
	dip->di_ckhash = 0;
	dip->di_ckhash = calculate_crc32c(~0L, (void *)dip, sizeof(*dip));
}

/*
 * These are the low-level functions that actually read and write
 * the superblock and its associated data.
 */
static off_t sblock_try[] = SBLOCKSEARCH;
static int readsuper(void *, struct fs **, off_t, int, int,
	int (*)(void *, off_t, void **, int));

/*
 * Read a superblock from the devfd device.
 *
 * If an alternate superblock is specified, it is read. Otherwise the
 * set of locations given in the SBLOCKSEARCH list is searched for a
 * superblock. Memory is allocated for the superblock by the readfunc and
 * is returned. If filltype is non-NULL, additional memory is allocated
 * of type filltype and filled in with the superblock summary information.
 * All memory is freed when any error is returned.
 *
 * If a superblock is found, zero is returned. Otherwise one of the
 * following error values is returned:
 *     EIO: non-existent or truncated superblock.
 *     EIO: error reading summary information.
 *     ENOENT: no usable known superblock found.
 *     ENOSPC: failed to allocate space for the superblock.
 *     EINVAL: The previous newfs operation on this volume did not complete.
 *         The administrator must complete newfs before using this volume.
 */
int
ffs_sbget(void *devfd, struct fs **fsp, off_t altsblock, int _filltype,
          int (*readfunc)(void *devfd, off_t loc, void **bufp, int size))
{
	struct fs *fs;
	struct fs_summary_info *fs_si;
	int i, error, size, blks;
	uint8_t *space;
	int32_t *lp;
	int chkhash;
	char *buf;

	fs = NULL;
	*fsp = NULL;
	if (altsblock >= 0) {
		if ((error = readsuper(devfd, &fs, altsblock, 1, 0, readfunc)) != 0) {
			if (fs != NULL)
                UFS_FREE(fs, _filltype);
			trace_return (error);
		}
	} else {
		chkhash = 1;
		if (altsblock == STDSB_NOHASHFAIL)
			chkhash = 0;
		for (i = 0; sblock_try[i] != -1; i++) {
			if ((error = readsuper(devfd, &fs, sblock_try[i], 0, chkhash, readfunc)) == 0)
				break;
			if (fs != NULL) {
                UFS_FREE(fs, _filltype);
				fs = NULL;
			}
			if (error == ENOENT)
				continue;
			trace_return (error);
		}
		if (sblock_try[i] == -1)
			trace_return (ENOENT);
	}
	/*
	 * Read in the superblock summary information.
	 */
	size = fs->fs_cssize;
	blks = howmany(size, fs->fs_fsize);
	if (fs->fs_contigsumsize > 0)
		size += fs->fs_ncg * sizeof(int32_t);
	size += fs->fs_ncg * sizeof(u_int8_t);
	/* When running in libufs or libsa, UFS_MALLOC may fail */
	if ((fs_si = UFS_MALLOC(sizeof(*fs_si), _filltype, M_WAITOK)) == NULL) {
        UFS_FREE(fs, _filltype);
		trace_return (ENOSPC);
	}
	bzero(fs_si, sizeof(*fs_si));
	fs->fs_si = fs_si;
	if ((space = UFS_MALLOC(size, _filltype, M_WAITOK)) == NULL) {
        UFS_FREE(fs->fs_si, _filltype);
        UFS_FREE(fs, _filltype);
		trace_return (ENOSPC);
	}
	fs->fs_csp = (struct csum *)space;
	for (i = 0; i < blks; i += fs->fs_frag) {
		size = fs->fs_bsize;
		if (i + fs->fs_frag > blks)
			size = (blks - i) * fs->fs_fsize;
		buf = NULL;
		error = (*readfunc)(devfd,
		    dbtob(fsbtodb(fs, fs->fs_csaddr + i), DEV_BSIZE), (void **)&buf, size);
		if (error) {
			if (buf != NULL)
                UFS_FREE(buf, _filltype); // TODO: check the size here.
			UFS_FREE(fs->fs_csp,_filltype);
            UFS_FREE(fs->fs_si, _filltype);
            UFS_FREE(fs, _filltype);
			trace_return (error);
		}
		memcpy(space, buf, size);
        UFS_FREE(buf, _filltype); // TODO: check the size here.
		space += size;
	}
	if (fs->fs_contigsumsize > 0) {
		fs->fs_maxcluster = lp = (int32_t *)space;
		for (i = 0; i < fs->fs_ncg; i++)
			*lp++ = fs->fs_contigsumsize;
		space = (uint8_t *)lp;
	}
	size = fs->fs_ncg * sizeof(u_int8_t);
	fs->fs_contigdirs = (u_int8_t *)space;
	bzero(fs->fs_contigdirs, size);
	*fsp = fs;
	trace_return (0);
}

/*
 * Try to read a superblock from the location specified by sblockloc.
 * Return zero on success or an errno on failure.
 */
static int
readsuper(void *devfd, struct fs **fsp, off_t sblockloc, int isaltsblk,
    int chkhash, int (*readfunc)(void *devfd, off_t loc, void **bufp, int size))
{
	struct fs *fs;
	int error, __unused res;
	uint32_t ckhash;

	error = (*readfunc)(devfd, sblockloc, (void **)fsp, SBLOCKSIZE);
	if (error != 0)
		trace_return (error);
	fs = *fsp;
	if (fs->fs_magic == FS_BAD_MAGIC)
		trace_return (EINVAL);
	if (((fs->fs_magic == FS_UFS1_MAGIC && (isaltsblk ||
	      sblockloc <= SBLOCK_UFS1)) ||
	     (fs->fs_magic == FS_UFS2_MAGIC && (isaltsblk ||
	      sblockloc == fs->fs_sblockloc))) &&
	    fs->fs_ncg >= 1 &&
	    fs->fs_bsize >= MINBSIZE &&
	    fs->fs_bsize <= MAXBSIZE &&
	    fs->fs_bsize >= roundup(sizeof(struct fs), DEV_BSIZE) &&
	    fs->fs_sbsize <= SBLOCKSIZE) {
		/*
		 * If the filesystem has been run on a kernel without
		 * metadata check hashes, disable them.
		 */
		if ((fs->fs_flags & FS_METACKHASH) == 0)
			fs->fs_metackhash = 0;
		/*
		 * Clear any check-hashes that are not maintained
		 * by this kernel. Also clear any unsupported flags.
		 */
		fs->fs_metackhash &= CK_SUPPORTED;
		fs->fs_flags &= FS_SUPPORTED;
		if (fs->fs_ckhash != (ckhash = ffs_calc_sbhash(fs))) {
			log_debug("Superblock check-hash failed: recorded "
			    "check-hash 0x%x != computed check-hash 0x%x%s\n",
			    fs->fs_ckhash, ckhash,
			    chkhash == 0 ? " (Ignored)" : "");
			if (chkhash == 0) {
				fs->fs_flags |= FS_NEEDSFSCK;
				fs->fs_fmod = 1;
				trace_return (0);
			}
			fs->fs_fmod = 0;
			trace_return (ESTALE);
		}
		/* Have to set for old filesystems that predate this field */
		fs->fs_sblockactualloc = sblockloc;
		/* Not yet any summary information */
		fs->fs_si = NULL;
		trace_return (0);
	}
    panic("UFS Magic mismatch MAGIC=%x, sblockloc=%lld", fs->fs_magic, sblockloc);
	trace_return (ENOENT);
}

/*
 * Write a superblock to the devfd device from the memory pointed to by fs.
 * Write out the superblock summary information if it is present.
 *
 * If the write is successful, zero is returned. Otherwise one of the
 * following error values is returned:
 *     EIO: failed to write superblock.
 *     EIO: failed to write superblock summary information.
 */
int
ffs_sbput(void *devfd, struct fs *fs, off_t loc,
    int (*writefunc)(void *devfd, off_t loc, void *buf, int size))
{
	int i, error, blks, size;
	uint8_t *space;
    off_t diskb;

	/*
	 * If there is summary information, write it first, so if there
	 * is an error, the superblock will not be marked as clean.
	 */
	if (fs->fs_si != NULL && fs->fs_csp != NULL) {
		blks = howmany(fs->fs_cssize, fs->fs_fsize);
		space = (uint8_t *)fs->fs_csp;
		for (i = 0; i < blks; i += fs->fs_frag) {
			size = fs->fs_bsize;
			if (i + fs->fs_frag > blks)
				size = (blks - i) * fs->fs_fsize;
            diskb = dbtob(fsbtodb(fs, fs->fs_csaddr + i), DEV_BSIZE);
			if ((error = (*writefunc)(devfd, diskb, space, size)) != 0)
				trace_return (error);
			space += size;
		}
	}
	fs->fs_fmod = 0;
#ifndef _KERNEL
	{
		struct fs_summary_info *fs_si;

		fs->fs_time = time(NULL);
		/* Clear the pointers for the duration of writing. */
		fs_si = fs->fs_si;
		fs->fs_si = NULL;
		fs->fs_ckhash = ffs_calc_sbhash(fs);
		error = (*writefunc)(devfd, loc, fs, fs->fs_sbsize);
		fs->fs_si = fs_si;
	}
#else /* _KERNEL */
	fs->fs_time = mach_absolute_time() / NSEC_PER_SEC;
	fs->fs_ckhash = ffs_calc_sbhash(fs);
	error = (*writefunc)(devfd, loc, fs, fs->fs_sbsize);
#endif /* _KERNEL */
	trace_return (error);
}

/*
 * Calculate the check-hash for a superblock.
 */
uint32_t
ffs_calc_sbhash(struct fs *fs)
{
	uint32_t ckhash, save_ckhash;

	/*
	 * A filesystem that was using a superblock ckhash may be moved
	 * to an older kernel that does not support ckhashes. The
	 * older kernel will clear the FS_METACKHASH flag indicating
	 * that it does not update hashes. When the disk is moved back
	 * to a kernel capable of ckhashes it disables them on mount:
	 *
	 *	if ((fs->fs_flags & FS_METACKHASH) == 0)
	 *		fs->fs_metackhash = 0;
	 *
	 * This leaves (fs->fs_metackhash & CK_SUPERBLOCK) == 0) with an
	 * old stale value in the fs->fs_ckhash field. Thus the need to
	 * just accept what is there.
	 */
	if ((fs->fs_metackhash & CK_SUPERBLOCK) == 0)
		trace_return (fs->fs_ckhash);

	save_ckhash = fs->fs_ckhash;
	fs->fs_ckhash = 0;
	/*
	 * If newly read from disk, the caller is responsible for
	 * verifying that fs->fs_sbsize <= SBLOCKSIZE.
	 */
	ckhash = calculate_crc32c(~0L, (void *)fs, fs->fs_sbsize);
	fs->fs_ckhash = save_ckhash;
	trace_return (ckhash);
}

/*
 * Update the frsum fields to reflect addition or deletion
 * of some frags.
 */
void
ffs_fragacct(struct fs *fs, int fragmap, int32_t fraglist[], int cnt)
{
	int inblk;
	int field, subfield;
	int siz, pos;

	inblk = (int)(fragtbl[fs->fs_frag][fragmap]) << 1;
	fragmap <<= 1;
	for (siz = 1; siz < fs->fs_frag; siz++) {
		if ((inblk & (1 << (siz + (fs->fs_frag % NBBY)))) == 0)
			continue;
		field = around[siz];
		subfield = inside[siz];
		for (pos = siz; pos <= fs->fs_frag; pos++) {
			if ((fragmap & field) == subfield) {
				fraglist[siz] += cnt;
				pos += siz;
				field <<= siz;
				subfield <<= siz;
			}
			field <<= 1;
			subfield <<= 1;
		}
	}
}

/*
 * block operations
 *
 * check if a block is available
 */
int
ffs_isblock(struct fs *fs, unsigned char *cp, ufs1_daddr_t h)
{
	unsigned char mask;

	switch ((int)fs->fs_frag) {
	case 8:
		trace_return (cp[h] == 0xff);
	case 4:
		mask = 0x0f << ((h & 0x1) << 2);
		trace_return ((cp[h >> 1] & mask) == mask);
	case 2:
		mask = 0x03 << ((h & 0x3) << 1);
		trace_return ((cp[h >> 2] & mask) == mask);
	case 1:
		mask = 0x01 << (h & 0x7);
		trace_return ((cp[h >> 3] & mask) == mask);
	default:
#ifdef _KERNEL
		panic("ffs_isblock");
#endif
		break;
	}
	trace_return (0);
}

/*
 * check if a block is free
 */
int
ffs_isfreeblock(struct fs *fs, u_char *cp, ufs1_daddr_t h)
{

	switch ((int)fs->fs_frag) {
	case 8:
		trace_return (cp[h] == 0);
	case 4:
		trace_return ((cp[h >> 1] & (0x0f << ((h & 0x1) << 2))) == 0);
	case 2:
		trace_return ((cp[h >> 2] & (0x03 << ((h & 0x3) << 1))) == 0);
	case 1:
		trace_return ((cp[h >> 3] & (0x01 << (h & 0x7))) == 0);
	default:
#ifdef _KERNEL
		panic("ffs_isfreeblock");
#endif
		break;
	}
	trace_return (0);
}

/*
 * take a block out of the map
 */
void
ffs_clrblock(struct fs *fs, u_char *cp, ufs1_daddr_t h)
{

	switch ((int)fs->fs_frag) {
	case 8:
		cp[h] = 0;
		return;
	case 4:
		cp[h >> 1] &= ~(0x0f << ((h & 0x1) << 2));
		return;
	case 2:
		cp[h >> 2] &= ~(0x03 << ((h & 0x3) << 1));
		return;
	case 1:
		cp[h >> 3] &= ~(0x01 << (h & 0x7));
		return;
	default:
#ifdef _KERNEL
		panic("ffs_clrblock");
#endif
		break;
	}
}

/*
 * put a block into the map
 */
void
ffs_setblock(struct fs *fs, unsigned char *cp, ufs1_daddr_t h)
{

	switch ((int)fs->fs_frag) {
	case 8:
		cp[h] = 0xff;
		return;
	case 4:
		cp[h >> 1] |= (0x0f << ((h & 0x1) << 2));
		return;
	case 2:
		cp[h >> 2] |= (0x03 << ((h & 0x3) << 1));
		return;
	case 1:
		cp[h >> 3] |= (0x01 << (h & 0x7));
		return;
	default:
#ifdef _KERNEL
		panic("ffs_setblock");
#endif
		break;
	}
}

/*
 * Update the cluster map because of an allocation or free.
 *
 * Cnt == 1 means free; cnt == -1 means allocating.
 */
void
ffs_clusteracct(struct fs *fs, struct cg *cgp, ufs1_daddr_t blkno, int cnt)
{
	int32_t *sump;
	int32_t *lp;
	u_char *freemapp, *mapp;
	int i, start, end, forw, back, map;
	u_int bit;

	if (fs->fs_contigsumsize <= 0)
		return;
	freemapp = cg_clustersfree(cgp);
	sump = cg_clustersum(cgp);
	/*
	 * Allocate or clear the actual block.
	 */
	if (cnt > 0)
		setbit(freemapp, blkno);
	else
		clrbit(freemapp, blkno);
	/*
	 * Find the size of the cluster going forward.
	 */
	start = blkno + 1;
	end = start + fs->fs_contigsumsize;
	if (end >= cgp->cg_nclusterblks)
		end = cgp->cg_nclusterblks;
	mapp = &freemapp[start / NBBY];
	map = *mapp++;
	bit = 1U << (start % NBBY);
	for (i = start; i < end; i++) {
		if ((map & bit) == 0)
			break;
		if ((i & (NBBY - 1)) != (NBBY - 1)) {
			bit <<= 1;
		} else {
			map = *mapp++;
			bit = 1;
		}
	}
	forw = i - start;
	/*
	 * Find the size of the cluster going backward.
	 */
	start = blkno - 1;
	end = start - fs->fs_contigsumsize;
	if (end < 0)
		end = -1;
	mapp = &freemapp[start / NBBY];
	map = *mapp--;
	bit = 1U << (start % NBBY);
	for (i = start; i > end; i--) {
		if ((map & bit) == 0)
			break;
		if ((i & (NBBY - 1)) != 0) {
			bit >>= 1;
		} else {
			map = *mapp--;
			bit = 1U << (NBBY - 1);
		}
	}
	back = start - i;
	/*
	 * Account for old cluster and the possibly new forward and
	 * back clusters.
	 */
	i = back + forw + 1;
	if (i > fs->fs_contigsumsize)
		i = fs->fs_contigsumsize;
	sump[i] += cnt;
	if (back > 0)
		sump[back] -= cnt;
	if (forw > 0)
		sump[forw] -= cnt;
	/*
	 * Update cluster summary information.
	 */
	lp = &sump[fs->fs_contigsumsize];
	for (i = fs->fs_contigsumsize; i > 0; i--)
		if (*lp-- > 0)
			break;
	fs->fs_maxcluster[cgp->cg_cgx] = i;
}
