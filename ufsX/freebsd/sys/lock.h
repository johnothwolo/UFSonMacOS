//
//  lock.h
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef vlock_h
#define vlock_h

#include <sys/lock.h>
#include <sys/vnode.h>

#pragma mark in case inode.h isn't included
#ifndef    VTOI
#define    VTOI(vp)    ((struct inode *)(vnode_fsnode(vp)))
#endif

#pragma mark in case ufs_vnops.h isn't included
#ifndef ufs_vnops_h
typedef int (*VOPFUNC)(void *);
#endif

#pragma mark below changes the vfs_busy LK_NOWAIT flag
#undef LK_NOWAIT
#ifndef MBF_NOWAIT
#define MBF_NOWAIT 1
#endif

struct inode;
struct vnode;
struct lock;
struct vfs_context;
typedef struct lock lock_t;


#define LCK_RW_ASSERT_SHARED        0x01
#define LCK_RW_ASSERT_EXCLUSIVE     0x02
#define LCK_RW_ASSERT_HELD          0x03
#define LCK_RW_ASSERT_NOTHELD       0x04

/*
 * lock flags
 */
typedef enum {
    LK_INIT_MASK    = 0x0001FF,
    LK_CANRECURSE   = 0x000001,
    LK_NODUP        = 0x000002,
    LK_NOPROFILE    = 0x000004,
    LK_NOSHARE      = 0x000008,
    LK_NOWITNESS    = 0x000010,
    LK_QUIET        = 0x000020,
    LK_UNUSED0      = 0x000040,    /* Was LK_ADAPTIVE */
    LK_IS_VNODE     = 0x000080,    /* Tell WITNESS about a VNODE lock */
    LK_NEW          = 0x000100,
    
    LK_TYPE_MASK    = 0xFF0000,
    LK_DOWNGRADE    = 0x010000,
    LK_DRAIN        = 0x020000,
    LK_EXCLOTHER    = 0x040000,
    LK_EXCLUSIVE    = 0x080000,
    LK_RELEASE      = 0x100000,
    LK_SHARED       = 0x200000,
    LK_UPGRADE      = 0x400000,
    LK_TRYUPGRADE   = 0x800000,
    
    /*
     * Additional attributes to be used in lockmgr().
     */
    LK_EATTR_MASK   = 0x00FF00,
    LK_INTERLOCK    = 0x000100,
    LK_NOWAIT       = 0x000200,
    LK_RETRY        = 0x000400,
    LK_SLEEPFAIL    = 0x000800,
    LK_TIMELOCK     = 0x001000,
    LK_NODDLKTREAT  = 0x002000,
    LK_ADAPTIVE     = 0x004000,
} vlock_flags_t;

void    assert_vi_locked(struct vnode *vp, const char *str);
void    assert_vi_unlocked(struct vnode *vp, const char *str);
void    assert_vop_elocked(struct vnode *vp, const char *str);
void    assert_vop_locked(struct vnode *vp, const char *str);
void    assert_vop_unlocked(struct vnode *vp, const char *str);

#define ASSERT_VOP_IN_SEQC(vp)        ((void)0)
#define ASSERT_VOP_NOT_IN_SEQC(vp)    ((void)0)

#define ASSERT_VI_LOCKED(vp, str)       assert_vi_locked((vp), (str))
#define ASSERT_VI_UNLOCKED(vp, str)     assert_vi_unlocked((vp), (str))
#define ASSERT_VNOP_ELOCKED(vp, str)    assert_vop_elocked((vp), (str))
#define ASSERT_VNOP_LOCKED(vp, str)     assert_vop_locked((vp), (str))
#define ASSERT_VNOP_UNLOCKED(vp, str)   assert_vop_unlocked((vp), (str))

// sets up/destroys i_lock and i_interlock
struct lock* std_newlock(void);
void   std_destroylock(struct lock*);

bool   islocked(struct inode *ip);
bool   islocked_exclusive(struct inode *ip);
int    std_lock(struct inode *ip, int flags, const char *file, int line);
void   std_unlock(struct inode *ip, int flags, const char *file, int line);
#define vn_lock(vp, flags)    VNOP_LOCK1((vp), flags, __FILE__, __LINE__)
#define vn_unlock(vp, flags)  VNOP_UNLOCK1((vp), __FILE__, __LINE__)

#define VI_LOCK(vp)           lck_mtx_lock(&VTOI(vp)->i_interlock)
#define VI_UNLOCK(vp)         lck_mtx_unlock(&VTOI(vp)->i_interlock)

#define VNOP_LOCK(vp, flags)  VNOP_LOCK1((vp), flags, __FILE__, __LINE__)
#define VNOP_UNLOCK(vp)       VNOP_UNLOCK1((vp), __FILE__, __LINE__)

struct vnop_islocked_args {
    struct vnode *a_vp;
};
int VOP_ISLOCKED_APV(VOPFUNC *__unused, struct vnop_islocked_args *);
int VOP_ISLOCKED(struct vnode *);

struct vnop_lock1_args {
    struct vnode *a_vp;
    const char *a_file;
    int a_line;
    int a_flags;
};
int VNOP_LOCK1(struct vnode *, int, const char *, int);
int VNOP_LOCK1_APV(VOPFUNC *__unused, struct vnop_lock1_args *);

struct vnop_unlock_args {
    struct vnode *a_vp;
    const char *a_file;
    int a_line;
};
int VNOP_UNLOCK1(struct vnode *, const char *, int);
int VNOP_UNLOCK_APV(VOPFUNC *__unused, struct vnop_unlock_args *);

#endif /* vlock_h */
