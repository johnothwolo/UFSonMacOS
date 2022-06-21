//
//  vnode.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef vnode_h
#define vnode_h

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
#define IO_NOMACCHECK   0
#define IO_NORMAL       0        /* operate on regular data */
#define IO_EXT          0        // vnode contains extended attr data

typedef int (*vn_get_ino_t)(struct mount *, void *, int, struct vnode **);


void    vn_seqc_write_begin(struct vnode *vp);
void    vn_seqc_write_end(struct vnode *vp);

int     vn_start_write(struct vnode *vp, struct mount **mpp, int flags);
void    vn_finished_write(struct mount *mp);
int     vn_start_secondary_write(struct vnode *vp, struct mount **mpp, int flags);
void    vn_finished_secondary_write(struct mount *mp);

void    vfs_write_resume(struct mount *mp, int flags);
int     vfs_write_suspend(struct mount *mp, int flags);
int     vfs_write_suspend_umnt(struct mount *mp);

void    vn_printf(struct vnode *vp, const char *fmt, ...);

int     vn_vget_ino(struct vnode *vp, ino_t ino, int lkflags,
                    struct vnode **rvp);
int     vn_vget_ino_gen(struct vnode *vp, vn_get_ino_t alloc,
                        void *alloc_arg, int lkflags, struct vnode **rvp);

/* vfs_hash.c */
typedef int vfs_hash_cmp_t(struct vnode *vp, void *arg);

int
vfs_hash_get(const struct mount *mp, u_int hash, int flags, struct thread *td,
             struct vnode **vpp, vfs_hash_cmp_t *fn, void *arg);
void
vfs_hash_remove(struct vnode *vp);
int
vfs_hash_insert(struct vnode *vp, u_int hash, int flags, struct thread *td,
                struct vnode **vpp, vfs_hash_cmp_t *fn, void *arg);
void
vfs_hash_rehash(struct vnode *vp, u_int hash);

#endif /* vnode_h */
