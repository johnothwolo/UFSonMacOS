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

#include <freebsd/sys/compat.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

static d_open_t  ffs_susp_open_close;
static d_write_t ffs_susp_rdwr;
static d_ioctl_t ffs_susp_ioctl;

static struct cdevsw ffs_susp_cdevsw = {
	.d_open      = ffs_susp_open_close,
    .d_close     = ffs_susp_open_close,
	.d_read      = ffs_susp_rdwr,
	.d_write     = ffs_susp_rdwr,
	.d_ioctl     = ffs_susp_ioctl,
    // unsupported options
    .d_stop      = (d_stop_t*)      enodev,
    .d_reset     = (d_reset_t*)     enodev,
    .d_ttys      = 0,
    .d_select    = (d_select_t*)    enodev,
    .d_mmap      = (d_mmap_t*)      enodev,
    .d_strategy  = (d_strategy_t*)  enodev,
    .d_type      = D_TTY,
    
};

static dev_t ffs_susp_dev = -1;
static int ffs_susp_cdev_major = -1;
static struct cdev *ffs_susp_cdev;
static lck_rw_t ffs_susp_lock;

static int
ffs_susp_suspended(struct mount *mp)
{
	struct ufsmount *ump;

    LCK_RW_ASSERT(ffs_susp_lock, LCK_RW_ASSERT_HELD);

	ump = VFSTOUFS(mp);
	if ((ump->um_flags & UM_WRITESUSPENDED) != 0)
		return (1);
	return (0);
}

static int
ffs_susp_open_close(dev_t dev, int flags, int devtype, struct proc *p)
{
	return (0);
}

static int
ffs_susp_rdwr(dev_t dev, struct uio *uio, int ioflag)
{
	int error, i;
	struct vnode *devvp;
	struct mount *mp;
	struct ufsmount *ump;
	struct buf *bp;
	user_addr_t base;
	user_size_t len;
	ssize_t cnt;
	struct fs *fs;
    int devBlockSize;

	lck_rw_lock_shared(&ffs_susp_lock);

	error = devfs_get_cdevpriv(dev, (void **)&mp);
	if (error != 0) {
		lck_rw_unlock_shared(&ffs_susp_lock);
		return (ENXIO);
	}

	ump = VFSTOUFS(mp);
	devvp = ump->um_devvp;
	fs = ump->um_fs;
    devBlockSize = vfs_devblocksize(ump->um_mountp);

	if (ffs_susp_suspended(mp) == 0) {
		lck_rw_unlock_shared(&ffs_susp_lock);
		return (ENXIO);
	}

	KASSERT(uio_rw(uio) == UIO_READ || uio_rw(uio) == UIO_WRITE,
	    ("neither UIO_READ or UIO_WRITE"));
	KASSERT(uio_isuserspace(uio), ("uio->uio_segflg != UIO_USERSPACE"));

	cnt = uio_resid(uio);

	for (i = 0; i < uio_iovcnt(uio); i++) {
        if(uio_getiov(uio, i, &base, &len))
            return EFAULT;
        
		while (len) {
			if (len > fs->fs_bsize)
				len = fs->fs_bsize;
			if (fragoff(fs, uio_offset(uio)) != 0 ||
			    fragoff(fs, len) != 0) {
				error = EINVAL;
				goto out;
			}
			error = buf_bread(devvp, btodb(uio_offset(uio), fs->fs_bsize), (int)len, NOCRED, &bp);
			if (error != 0)
				goto out;
			if (uio_rw(uio) == UIO_WRITE) {
				error = copyin(base, (void*)buf_dataptr(bp), len);
				if (error != 0) {
                    buf_markinvalid(bp);
					buf_brelse(bp);
					goto out;
				}
				error = buf_bwrite(bp);
				if (error != 0)
					goto out;
			} else {
				error = copyout((void*)buf_dataptr(bp), base, len);
				buf_brelse(bp);
				if (error != 0)
					goto out;
			}
            uio_update(uio, len);
			uio_setresid(uio, uio_resid(uio)- len);
            uio_setoffset(uio, uio_offset(uio) + len);
		}
	}

out:
	lck_rw_unlock_shared(&ffs_susp_lock);

	if (uio_resid(uio) < cnt)
		return (0);

	return (error);
}

static int
ffs_susp_suspend(struct mount *mp, struct proc *p)
{
	struct ufsmount *ump;
    vfs_context_t context;
    struct ucred *ccred, *pcred;
	int error;

	LCK_RW_ASSERT(ffs_susp_lock, LCK_RW_ASSERT_EXCLUSIVE);

	if (!ffs_own_mount(mp))
		return (EINVAL);
	if (ffs_susp_suspended(mp))
		return (EBUSY);

	ump = VFSTOUFS(mp);
    context = vfs_context_current();
    ccred = vfs_context_ucred(context);
    pcred = proc_ucred(p);
	/*
	 * Make sure the calling thread is permitted to access the mounted
	 * device.  The permissions can change after we unlock the vnode;
	 * it's harmless.
	 */
    KASSERT(ccred->cr_posix.cr_uid == pcred->cr_posix.cr_uid &&
            pcred->cr_posix.cr_gmuid == pcred->cr_posix.cr_gmuid,
            ("VFS_CONTEXT mismatch"));
    
    error = vnode_authorize(ump->um_odevvp, NULLVP,
                            KAUTH_VNODE_READ_DATA|KAUTH_VNODE_WRITE_DATA, context);
    
	if (error != 0)
		return (error);

	if ((error = vfs_write_suspend(mp, VS_SKIP_UNMOUNT)) != 0)
		return (error);

	UFS_LOCK(ump);
	ump->um_flags |= UM_WRITESUSPENDED;
	UFS_UNLOCK(ump);

	return (0);
}

static void
ffs_susp_unsuspend(struct mount *mp)
{
	struct ufsmount *ump;
    ump = VFSTOUFS(mp);

	LCK_RW_ASSERT(ffs_susp_lock, LCK_RW_ASSERT_EXCLUSIVE);

	/*
	 * XXX: The status is kept per-process; the vfs_write_resume() routine
	 * 	asserts that the resuming thread is the same one that called
	 * 	vfs_write_suspend().  The cdevpriv data, however, is attached
	 * 	to the file descriptor, e.g. is inherited during fork.  Thus,
	 * 	it's possible that the resuming process will be different from
	 * 	the one that started the suspension.
	 *
	 * 	Work around by fooling the check in vfs_write_resume().
	 */
	ump->um_susp_owner = current_thread();

	vfs_write_resume(mp, 0);
	UFS_LOCK(ump);
	ump->um_flags &= ~UM_WRITESUSPENDED;
	UFS_UNLOCK(ump);
	vfs_unbusy(mp);
}

static void
ffs_susp_dtor(void *data)
{
	struct fs *fs;
	struct ufsmount *ump;
	struct mount *mp;
	int error;

	lck_rw_lock_exclusive(&ffs_susp_lock);

	mp = (struct mount *)data;
	ump = VFSTOUFS(mp);
	fs = ump->um_fs;

	if (ffs_susp_suspended(mp) == 0) {
		lck_rw_unlock_exclusive(&ffs_susp_lock);
		return;
	}

	KASSERT((ump->um_kern_flag & MNTK_SUSPEND) != 0,
	    ("MNTK_SUSPEND not set"));

	error = ffs_reload(mp, current_thread(), FFSR_FORCE | FFSR_UNSUSPEND);
	if (error != 0)
		panic("failed to unsuspend writes on %s", fs->fs_fsmnt);

	ffs_susp_unsuspend(mp);
	lck_rw_unlock_exclusive(&ffs_susp_lock);
}

static int
ffs_susp_ioctl(dev_t dev, u_long cmd, caddr_t addr, int fflag, struct proc *p)
{
	struct mount *mp;
	fsid_t *fsidp;
	int error;

	lck_rw_lock_exclusive(&ffs_susp_lock);

	switch (cmd) {
	case UFSSUSPEND:
		fsidp = (fsid_t *)addr;
		mp = vfs_getvfs(fsidp);
		if (mp == NULL) {
			error = ENOENT;
			break;
		}
		error = vfs_busy_fbsd(mp, 0);
		vfs_rel(mp);
		if (error != 0)
			break;
		error = ffs_susp_suspend(mp, p);
		if (error != 0) {
			vfs_unbusy(mp);
			break;
		}
		error = devfs_set_cdevpriv(dev, mp, ffs_susp_dtor);
		if (error != 0)
			ffs_susp_unsuspend(mp);
		break;
	case UFSRESUME:
		error = devfs_get_cdevpriv(dev, (void **)&mp);
		if (error != 0)
			break;
		/*
		 * This calls ffs_susp_dtor, which in turn unsuspends the fs.
		 * The dtor expects to be called without lock held, because
		 * sometimes it's called from here, and sometimes due to the
		 * file being closed or process exiting.
		 */
		lck_rw_unlock_exclusive(&ffs_susp_lock);
		devfs_clear_cdevpriv(ffs_susp_dev);
		return (0);
	default:
		error = ENXIO;
		break;
	}

	lck_rw_unlock_exclusive(&ffs_susp_lock);

	return (error);
}

void
ffs_susp_initialize(void)
{

	lck_rw_init(&ffs_susp_lock, LCK_GRP_NULL, LCK_ATTR_NULL);
    
    ffs_susp_cdev_major = cdevsw_add(-24, &ffs_susp_cdevsw);;
    ffs_susp_dev = makedev(ffs_susp_cdev_major, 0);
    
    ffs_susp_cdev = devfs_make_node(ffs_susp_dev,
                                    DEVFS_CHAR,
                                    UID_ROOT,
                                    GID_WHEEL,
                                    0666,
                                    "ufssuspend");
}

void
ffs_susp_uninitialize(void)
{
    int ret;
    
    devfs_remove(ffs_susp_cdev);
    ret = cdevsw_remove(ffs_susp_cdev_major, &ffs_susp_cdevsw);
    if (ret != ffs_susp_cdev_major) {
        ufs_debug("ffs_susp: ffs_susp_cdev_major != return from cdevsw_remove()\n");
    }
    ffs_susp_cdev_major = -1;
	lck_rw_destroy(&ffs_susp_lock, LCK_GRP_NULL);
}
