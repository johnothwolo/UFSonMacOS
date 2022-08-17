/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2012 The FreeBSD Foundation
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
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
 * $FreeBSD$
 */

#ifndef MACH_ASSERT
#define MACH_ASSERT 1
#endif

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/ioccom.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/kauth.h>

#include <miscfs/devfs/devfs.h>

#include <freebsd/compat/compat.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

extern lck_grp_t *ffs_lock_group;
static lck_rw_t *ffs_susp_lock;

static int
ffs_susp_suspended(struct mount *mp)
{
    struct ufsmount *ump;

    ump = VFSTOUFS(mp);
    if ((ump->um_flags & UM_WRITESUSPENDED) != 0)
        return (1);
    return (0);
}

static int
ffs_susp_suspend(struct mount *mp, struct proc *p)
{
    struct ufsmount *ump;
    vfs_context_t context;
    struct proc *context_proc;
    struct ucred *ccred;
    int error;

    if (ffs_susp_suspended(mp))
        return (EBUSY);

    ump = VFSTOUFS(mp);
    context = vfs_context_current();
    ccred = vfs_context_ucred(context);
    context_proc = vfs_context_proc(context);
    /*
     * Make sure the calling thread is permitted to access the mounted
     * device.  The permissions can change after we unlock the vnode;
     * it's harmless.
     */
    ASSERT(context && context_proc == p, ("VFS_CONTEXT mismatch"));
    
    error = vnode_authorize(ump->um_devvp, NULLVP,
                            KAUTH_VNODE_READ_DATA|KAUTH_VNODE_WRITE_DATA, context);
    
    if (error != 0)
        return (error);

    if ((error = vfs_write_suspend(mp, 0, VS_SKIP_UNMOUNT)) != 0)
        return (error);

    UFS_LOCK(ump);
    ump->um_flags |= UM_WRITESUSPENDED;
    UFS_UNLOCK(ump);

    return (0);
}

static void
ffs_susp_unsuspend(struct mount *mp)
{
    struct ufsmount *ump = VFSTOUFS(mp);

    /*
     * XXX: The status is kept per-process; the vfs_write_resume() routine
     *     asserts that the resuming thread is the same one that called
     *     vfs_write_suspend(). The cdevpriv data, however, is attached
     *     to the file descriptor, e.g. is inherited during fork. Thus,
     *     it's possible that the resuming process will be different from
     *     the one that started the suspension.
     *
     *     Work around by fooling the check in vfs_write_resume().
     */
    vfs_set_susupendowner(mp, current_thread());
    vfs_write_resume(mp, 0);
    UFS_LOCK(ump);
    ump->um_flags &= ~UM_WRITESUSPENDED;
    UFS_UNLOCK(ump);
    vfs_unbusy(mp);
}

static int
ffs_susp_dtor(void *data)
{
    struct fs *fs;
    struct bsdmount *bmp;
    struct ufsmount *ump;
    struct mount *mp;
    int error;


    mp = (struct mount *)data;
    ump = VFSTOUFS(mp);
    bmp = ump->um_compat;
    fs = ump->um_fs;

    if (ffs_susp_suspended(mp) == 0) {
        return ENOENT;
    }

    ASSERT((bmp->mnt_kern_flag & MNTK_SUSPEND) != 0, ("MNTK_SUSPEND not set"));

    error = ffs_reload(mp, vfs_context_current(), FFSR_FORCE | FFSR_UNSUSPEND);
    if (error != 0){
        log_debug("failed to unsuspend writes on %s", fs->fs_fsmnt);
        return error;
    }
    ffs_susp_unsuspend(mp);
    return 0;
}

int
ffs_susp_ioctl(struct vnop_ioctl_args *ap)
{
    struct mount *mp, *mp0;
    struct vnode *vp;
    struct proc *p;
    fsid_t *fsidp;
    int error;

    lck_rw_lock_exclusive(ffs_susp_lock);

    p = vfs_context_proc(ap->a_context);
    vp = ap->a_vp;
    mp0 = vnode_mount(vp);
    switch (ap->a_command) {
        case FSCTL_UFSSUSPEND:
            fsidp = (fsid_t *)ap->a_data;
            mp = vfs_getvfs(fsidp);
            if (mp != NULL) {
                // check fs just as ffs_own_mount() does
                if (mp != mp0)
                    return (ENOENT);
            } else {
                log_debug("[WARNING]: no argument was found, falling back to vnode_mount");
                mp = mp0;
            }
            
            error = vfs_busy_bsd(mp, 0);
            vfs_rel(mp);
            if (error != 0)
                break;
            error = ffs_susp_suspend(mp, p);
            if (error != 0) {
                vfs_unbusy(mp);
                break;
            };
            break;
        case FSCTL_UFSRESUME:
            return ffs_susp_dtor(mp0);
        default:
            panic("ffs_susp_ioctl(): We shouldn't reach here");
            __builtin_unreachable();
    }

    lck_rw_unlock_exclusive(ffs_susp_lock);

    return (error);
}

void
ffs_susp_initialize(void)
{
    lck_rw_init(ffs_susp_lock, ffs_lock_group, LCK_ATTR_NULL);
}

void
ffs_susp_uninitialize(void)
{
    lck_rw_destroy(ffs_susp_lock, ffs_lock_group);
}
