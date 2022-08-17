/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright 1998, 2000 Marshall Kirk McKusick.
 * Copyright 2009, 2010 Jeffrey W. Roberson <jeff@FreeBSD.org>
 * All rights reserved.
 *
 * The soft updates code is derived from the appendix of a University
 * of Michigan technical report (Gregory R. Ganger and Yale N. Patt,
 * "Soft Updates: A Solution to the Metadata Update Problem in File
 * Systems", CSE-TR-254-95, August 1995).
 *
 * Further information about soft updates can be obtained from:
 *
 *    Marshall Kirk McKusick        http://www.mckusick.com/softdep/
 *    1614 Oxford Street        mckusick@mckusick.com
 *    Berkeley, CA 94709-1608        +1-510-843-9542
 *    USA
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *    from: @(#)ffs_softdep.c    9.59 (McKusick) 6/21/00
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

// #include "opt_ffs.h"
// #include "opt_quota.h"
// #include "opt_ddb.h"

#define MACH_ASSERT                 1 // for LCK_RW_ASSERT(a,b)
#define LCK_RW_ASSERT_EXCLUSIVE     0x02


#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/ubc.h>
#include <sys/disk.h>

#include <kern/sched_prim.h>
#include <freebsd/compat/compat.h>

#include <ufs/ufs/dir.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ffs/fs.h>
#include <ufs/ffs/softdep.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ufs/ufs_extern.h>



#ifndef SOFTUPDATES

int softupdates_enabled = 0;

int
softdep_flushfiles(
    struct mount *oldmnt,
    int flags,
    struct vfs_context *ctx)
{

    panic("softdep_flushfiles called");
    __builtin_unreachable();
}

int
softdep_mount(devvp, mp, fs, ctx)
    struct vnode *devvp;
    struct mount *mp;
    struct fs *fs;
    struct vfs_context *ctx;
{

    return (0);
}

void
softdep_initialize()
{
    log_debug("enter");
    return;
}

void
softdep_uninitialize()
{

    return;
}

void
softdep_unmount(mp)
    struct mount *mp;
{

    panic("softdep_unmount called");
}

void
softdep_setup_sbupdate(ump, fs, bp)
    struct ufsmount *ump;
    struct fs *fs;
    struct buf *bp;
{

    panic("softdep_setup_sbupdate called");
}

void
softdep_setup_inomapdep(bp, ip, newinum, mode)
    struct buf *bp;
    struct inode *ip;
    ino_t newinum;
    int mode;
{

    panic("softdep_setup_inomapdep called");
}

void
softdep_setup_blkmapdep(bp, mp, newblkno, frags, oldfrags)
    struct buf *bp;
    struct mount *mp;
    ufs2_daddr_t newblkno;
    int frags;
    int oldfrags;
{

    panic("softdep_setup_blkmapdep called");
}

void
softdep_setup_allocdirect(ip, lbn, newblkno, oldblkno, newsize, oldsize, bp)
    struct inode *ip;
    ufs_lbn_t lbn;
    ufs2_daddr_t newblkno;
    ufs2_daddr_t oldblkno;
    long newsize;
    long oldsize;
    struct buf *bp;
{

    panic("softdep_setup_allocdirect called");
}

void
softdep_setup_allocext(ip, lbn, newblkno, oldblkno, newsize, oldsize, bp)
    struct inode *ip;
    ufs_lbn_t lbn;
    ufs2_daddr_t newblkno;
    ufs2_daddr_t oldblkno;
    long newsize;
    long oldsize;
    struct buf *bp;
{

    panic("softdep_setup_allocext called");
}

void
softdep_setup_allocindir_page(ip, lbn, bp, ptrno, newblkno, oldblkno, nbp)
    struct inode *ip;
    ufs_lbn_t lbn;
    struct buf *bp;
    int ptrno;
    ufs2_daddr_t newblkno;
    ufs2_daddr_t oldblkno;
    struct buf *nbp;
{

    panic("softdep_setup_allocindir_page called");
}

void
softdep_setup_allocindir_meta(nbp, ip, bp, ptrno, newblkno)
    struct buf *nbp;
    struct inode *ip;
    struct buf *bp;
    int ptrno;
    ufs2_daddr_t newblkno;
{

    panic("softdep_setup_allocindir_meta called");
}

void
softdep_journal_freeblocks(ip, cred, length, flags)
    struct inode *ip;
    struct ucred *cred;
    off_t length;
    int flags;
{

    panic("softdep_journal_freeblocks called");
}

void
softdep_journal_fsync(ip)
    struct inode *ip;
{

    panic("softdep_journal_fsync called");
}

void
softdep_setup_freeblocks(ip, length, flags)
    struct inode *ip;
    off_t length;
    int flags;
{

    panic("softdep_setup_freeblocks called");
}

void
softdep_freefile(pvp, ino, mode)
        struct vnode *pvp;
        ino_t ino;
        int mode;
{

    panic("softdep_freefile called");
}

int
softdep_setup_directory_add(bp, dp, diroffset, newinum, newdirbp, isnewblk)
    struct buf *bp;
    struct inode *dp;
    off_t diroffset;
    ino_t newinum;
    struct buf *newdirbp;
    int isnewblk;
{

    panic("softdep_setup_directory_add called");
    __builtin_unreachable();
}

void
softdep_change_directoryentry_offset(bp, dp, base, oldloc, newloc, entrysize)
    struct buf *bp;
    struct inode *dp;
    caddr_t base;
    caddr_t oldloc;
    caddr_t newloc;
    int entrysize;
{

    panic("softdep_change_directoryentry_offset called");
}

void
softdep_setup_remove(bp, dp, ip, isrmdir)
    struct buf *bp;
    struct inode *dp;
    struct inode *ip;
    int isrmdir;
{

    panic("softdep_setup_remove called");
}

void
softdep_setup_directory_change(bp, dp, ip, newinum, isrmdir)
    struct buf *bp;
    struct inode *dp;
    struct inode *ip;
    ino_t newinum;
    int isrmdir;
{

    panic("softdep_setup_directory_change called");
}

void
softdep_setup_blkfree(mp, bp, blkno, frags, wkhd)
    struct mount *mp;
    struct buf *bp;
    ufs2_daddr_t blkno;
    int frags;
    struct workhead *wkhd;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_inofree(mp, bp, ino, wkhd)
    struct mount *mp;
    struct buf *bp;
    ino_t ino;
    struct workhead *wkhd;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_unlink(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_link(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_revert_link(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_rmdir(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_revert_rmdir(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_create(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_revert_create(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_mkdir(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_revert_mkdir(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

void
softdep_setup_dotdot_link(dp, ip)
    struct inode *dp;
    struct inode *ip;
{

    panic("%s called", __FUNCTION__);
}

int
softdep_prealloc(vp, waitok)
    struct vnode *vp;
    int waitok;
{

    panic("%s called", __FUNCTION__);
    __builtin_unreachable();
}

int
softdep_journal_lookup(mp, vpp)
    struct mount *mp;
    struct vnode **vpp;
{

    return (ENOENT);
}

void
softdep_change_linkcnt(ip)
    struct inode *ip;
{

    panic("softdep_change_linkcnt called");
}

void
softdep_load_inodeblock(ip)
    struct inode *ip;
{

    panic("softdep_load_inodeblock called");
}

void
softdep_update_inodeblock(ip, bp, waitfor)
    struct inode *ip;
    struct buf *bp;
    int waitfor;
{

    panic("softdep_update_inodeblock called");
}

int
softdep_fsync(vp)
    struct vnode *vp;    /* the "in_core" copy of the inode */
{

    return (0);
}

void
softdep_fsync_mountdev(vp)
    struct vnode *vp;
{

    return;
}

int
softdep_flushworklist(oldmnt, countp, ctx)
    struct mount *oldmnt;
    int *countp;
    struct vfs_context *ctx;
{

    *countp = 0;
    return (0);
}

int
softdep_sync_metadata(struct vnode *vp)
{

    panic("softdep_sync_metadata called");
    __builtin_unreachable();
}

int
softdep_sync_buf(struct vnode *vp, struct buf *bp, int waitfor)
{

    panic("softdep_sync_buf called");
    __builtin_unreachable();
}

int
softdep_slowdown(vp)
    struct vnode *vp;
{

    panic("softdep_slowdown called");
    __builtin_unreachable();
}

int
softdep_request_cleanup(fs, vp, cred, resource)
    struct fs *fs;
    struct vnode *vp;
    struct ucred *cred;
    int resource;
{

    return (0);
}

int
softdep_check_suspend(struct mount *mp,
              struct vnode *devvp,
              int softdep_depcnt,
              int softdep_accdepcnt,
              int secondary_writes,
              int secondary_accwrites)
{
    struct ufsmount *ump;
    struct bsdmount *bmp;
    int error;

    (void) softdep_depcnt,
    (void) softdep_accdepcnt;

    ump = VFSTOUFS(mp);
    bmp = ump->um_compat;

    MNT_ILOCK(bmp);
    while (bmp->mnt_secondary_writes != 0) {
        msleep(&bmp->mnt_secondary_writes, MNT_MTX(bmp), (PUSER - 1) | PDROP, "secwr", 0);
        MNT_ILOCK(bmp);
    }

    /*
     * Reasons for needing more work before suspend:
     * - Dirty buffers on devvp.
     * - Secondary writes occurred after start of vnode sync loop
     */
    error = 0;
    if (vnode_hasdirtyblks(devvp) ||
        secondary_writes != 0 ||
        bmp->mnt_secondary_writes != 0 ||
        secondary_accwrites != bmp->mnt_secondary_accwrites)
        error = EAGAIN;

    return (error);
}

void
softdep_get_depcounts(struct mount *mp,
              int *softdepactivep,
              int *softdepactiveaccp)
{
    (void) mp;
    *softdepactivep = 0;
    *softdepactiveaccp = 0;
}

void
softdep_buf_append(bp, wkhd)
    struct buf *bp;
    struct workhead *wkhd;
{

    panic("softdep_buf_appendwork called");
}

void
softdep_inode_append(ip, cred, wkhd)
    struct inode *ip;
    struct ucred *cred;
    struct workhead *wkhd;
{

    panic("softdep_inode_appendwork called");
}

void
softdep_freework(wkhd)
    struct workhead *wkhd;
{

    panic("softdep_freework called");
}

int
softdep_prerename(fdvp, fvp, tdvp, tvp)
    struct vnode *fdvp;
    struct vnode *fvp;
    struct vnode *tdvp;
    struct vnode *tvp;
{

    panic("softdep_prerename called");
    __builtin_unreachable();
}

int
softdep_prelink(dvp, vp, will_direnter)
    struct vnode *dvp;
    struct vnode *vp;
    int will_direnter;
{

    panic("softdep_prelink called");
    __builtin_unreachable();
}

#endif
