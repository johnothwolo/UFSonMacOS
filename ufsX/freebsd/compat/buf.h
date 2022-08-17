//
//  buf.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef buf_h
#define buf_h

#include <sys/buf.h>

struct bio;
struct bsdbuf;
struct bufobj;
struct mount;
struct vnode;
struct uio;

typedef void (*ckhashfunc_t)(struct buf *);
typedef uint32_t b_xflags_t;

// sysctl node for the freebsd fs subsys
extern struct sysctl_oid sysctl__vfs_freebsd;

#define B_INVAL         0x00040000      /* Does not contain valid info. */
#define B_CLUSTEROK     0

#pragma mark -
/*
 * To avoid including <ufs/ffs/softdep.h>
 */
LIST_HEAD(workhead, worklist);
/*
 * These are currently used only by the soft dependency code, hence
 * are stored once in a global variable. If other subsystems wanted
 * to use these hooks, a pointer to a set of bio_ops could be added
 * to each buffer.
 */
extern struct bio_ops {
    void    (*io_start)(struct buf *);
    void    (*io_complete)(struct buf *);
    void    (*io_deallocate)(struct buf *);
    int     (*io_countdeps)(struct buf *, int);
} bioops;

#pragma mark -

// bp is for buf_private
struct bufpriv {
    uint64_t             b_ckhash;          /* B_CKHASH requested check-hash */
    uint64_t             b_cflags;          /* bufpriv-compatible freebsd flags */
    uint64_t             b_vflags;
    uint64_t             b_xflags;
    struct buf          *b_bp;              /* 'owner' buffer */
    struct workhead      b_dep;             /* (D) List of filesystem dependencies. */
    void                *b_fsprivate1;
    void                *b_fsprivate2;
    void                *b_fsprivate3;
};

struct buf* incore(struct vnode *vp, daddr64_t bn);

/*
 * Flags for getblkx's last parameter.
 */
#define    GB_UFS_LOCK_NOWAIT   0x0001        /* Fail if we block on a buf lock. */
#define    GB_NOCREAT       0x0002        /* Don't create a buf if not found. */
#define    GB_NOWAIT_BD     0x0004        /* Do not wait for bufdaemon. */
#define    GB_UNMAPPED      0x0008        /* Do not mmap buffer pages. */
#define    GB_KVAALLOC      0x0010        /* But allocate KVA. */
#define    GB_CKHASH        0x0020        /* If reading, calc checksum hash */
#define    GB_NOSPARSE      0x0040        /* Do not instantiate holes */
#define    GB_CVTENXIO      0x0080        /* Convert errors to ENXIO */

/*
 * These flags are kept in buf_fsprivate().
 */

// private 'x' flags for buf_t
// freebsd says they're fs-defined flags
#define    BX_VNDIRTY       0x00000001    /* On vnode dirty list */
#define    BX_VNCLEAN       0x00000002    /* On vnode clean list */
#define    BX_CVTENXIO      0x00000004    /* Convert errors to ENXIO */
#define    BX_BKGRDWRITE    0x00000010    /* Do writes in background */
#define    BX_BKGRDMARKER   0x00000020    /* Mark buffer for splay tree */
#define    BX_ALTDATA       0x00000040    /* Holds extended data */
#define    BX_FSPRIV        0x00FF0000    /* Filesystem-specific flags mask */
#define    BX_XFLAGMASK     0x00FF00FF    /* BX_* flags mask */

// freebsd buf BV_* flags
#define    BV_SCANNED       0x0100000    /* VOP_FSYNC funcs mark written bufs */
#define    BV_BKGRDINPROG   0x0200000    /* Background write in progress */
#define    BV_BKGRDWAIT     0x0400000    /* Background write waiting */
#define    BV_BKGRDERR      0x0800000    /* Error from background write */
#define    BX_VFLAGMASK     0xFF00000    /* BV_* flags mask */

// buf BC_* flags. BC is for bufpriv-compatible
#define    BC_CKHASH        0x00000100    /* checksum hash calculated on read */

bool buf_checked_hash(buf_t bp);
uint64_t buf_ckhash_value(buf_t bp);

// BX_* flags
int  buf_xflags(buf_t bp);
void buf_clearxflags(buf_t bp, int falgs);
void buf_setxflags(buf_t bp, int flags);
// BV_* flags
int  buf_vflags(buf_t bp);
void buf_clearvflags(buf_t bp, int falgs);
void buf_setvflags(buf_t bp, int flags);
// MARK: bwrite() whould be defined and redirected to fileystem bwrite() (i.e. ffs_bufwrite())
// or just redirected to buf_bwrite()
int bwrite(buf_t bp);

// mark a buffer as B_DELWI and clear invalid flag,
// so that it can be re-queued by buf_brelse.
static inline void
buf_qrelse(buf_t bp)
{
    buf_markdelayed(bp);
    buf_brelse(bp);
}

/*
 * Flags for breadn_flags
 */
#define     B_NOWAIT 0x00008000      /* Buffer is likely to remain unaltered */

int
breadn_flags(struct vnode *vp, daddr64_t blkno, daddr64_t dblkno, int size,
             daddr64_t *rablkno, int *rabsize, int cnt,
             struct ucred *cred, int flags, ckhashfunc_t, struct buf **bpp);

int
meta_bread_flags(struct vnode *vp, daddr64_t blkno, int size,
                 struct ucred *cred, int flags, ckhashfunc_t, struct buf **bpp);

buf_t getblk(vnode_t vp, daddr64_t blkno, int size,
             int slpflag, int slptimeo, int operation);

#define bread(vp, blkno, size, cred, flags, bpp) \
            breadn_flags(vp, blkno, blkno, size, NULL, NULL, 0, cred, flags, NULL, bpp)

static inline void
buf_start(struct buf *bp)
{
    if (bioops.io_start)
        (*bioops.io_start)(bp);
}

static inline void
buf_complete(struct buf *bp)
{
    if (bioops.io_complete)
        (*bioops.io_complete)(bp);
}

static inline void
buf_deallocate(struct buf *bp)
{
    if (bioops.io_deallocate)
        (*bioops.io_deallocate)(bp);
}

static inline int
buf_countdeps(struct buf *bp, int i)
{
    if (bioops.io_countdeps)
        return ((*bioops.io_countdeps)(bp, i));
    else
        return (0);
}

#endif /* buf_h */
