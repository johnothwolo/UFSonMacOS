//
//  vnode.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef vnode_h
#define vnode_h

#include <sys/vnode.h>

struct bsdmount;

#define DOINGASYNC(vp)  (!vfs_issynchronous(vnode_mount(vp)))

#define VN_IS_DOOMED(vp) vnode_isrecycled((vp)) // check if vnode's about to be destroyed

#define vlazy(x)        ((void)0)

#define V_WAIT          0x0001    /* vn_start_write: sleep for suspend */
#define V_NOWAIT        0x0002    /* vn_start_write: don't sleep for suspend */
#define V_XSLEEP        0x0004    /* vn_start_write: just return after sleep */
#define V_MNTREF        0x0010    /* vn_start_write: mp is already ref-ed */

#define VR_START_WRITE  0x0001    /* vfs_write_resume: start write atomically */
#define VR_NO_SUSPCLR   0x0002    /* vfs_write_resume: do not clear suspension */

#define VS_SKIP_UNMOUNT 0x0001    /* vfs_write_suspend: fail if the
                                     filesystem is being unmounted */

// MARK: it's safe to just OR these, XNU will filter them out.
// MARK: However, we may need to update our flags if they overlap.
// MARK: BUT if that happens we may not have any leg room;
#define FREEBSD_IO_NOMACCHECK   0x10000000
#define FREEBSD_IO_NORMAL       0x20000000        /* operate on regular data */
#define FREEBSD_IO_EXT          0x40000000        // vnode contains extended attr data
#define FREEBSD_IO_BUFLOCKED    0x80000000

#define EARLYFLUSH      0x0008            /* vflush: early call for ffs_flushfiles */

typedef int (*vn_get_ino_t)(struct mount *, void *, int, struct vnode **);

#define ITOVCOMPAT(ip)   (&(ip)->cvnode)

/* MARK: This should be the first member in the inode struct just like netbsd's genfs_node */
struct vnode_compat {
//    struct vnlock_ops ops;
    struct bsdmount    *v_bsdmount;
    int dummy;
};

int     vn_start_write(struct vnode *vp, struct mount **mpp, int flags);
void    vn_finished_write(struct mount *mp);
int     vn_start_secondary_write(struct vnode *vp, struct mount **mpp, int flags);
void    vn_finished_secondary_write(struct mount *mp);

int     vfs_set_susupendowner(struct mount *mp, thread_t td);
void    vfs_write_resume(struct mount *mp, int flags);
int     vfs_write_suspend(struct mount *mp, thread_t td, int flags);
int     vfs_write_suspend_umnt(struct mount *mp);

void  __vn_printf(struct vnode *, thread_t, const char*, int, const char*, const char*, ...);
#define vn_printf(vp, fmt, a...) __vn_printf(vp, current_thread(), __FILE__, __LINE__, __func__, fmt, ##a)

int     vn_vget_ino(struct vnode *vp, ino_t ino, int lkflags,
                    struct vnode **rvp);
int     vn_vget_ino_gen(struct vnode *vp, vn_get_ino_t alloc,
                        void *alloc_arg, int lkflags, struct vnode **rvp);

/* filesystem has to define these functions and redirect them to *vnop_* */
errno_t VNOP_CREATE(struct vnode*, struct vnode**, struct componentname *,
                    struct vnode_attr *, struct vfs_context *);

errno_t VNOP_MKDIR(vnode_t, vnode_t *, struct componentname *,
                   struct vnode_attr *, vfs_context_t);

#endif /* vnode_h */
