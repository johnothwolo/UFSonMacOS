//
//  buf_compat.c
//  ufsX
//
//  Created by John Othwolo on 7/27/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/ucred.h>

#include <freebsd/compat/compat.h>

extern int version_major;
extern int version_minor;
static const int B_FILTER = 0x00400000;

struct    bio_ops bioops;        /* I/O operation notification */

struct bufpriv bprivq[10240] = {0};

struct bufpriv*
bufpriv_create(struct buf *bp)
{
    struct bufpriv *bpriv = malloc(sizeof(struct bufpriv), M_TEMP, M_ZERO);
    LIST_INIT(&bpriv->b_dep);
    bpriv->b_bp = bp;
    buf_setfsprivate(bp, bpriv);
    return bpriv;
}

static void
bufpriv_detach_free(struct buf *bp)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    assert(bpriv != NULL); // let's make sure the code logic flows like butter.
    buf_setfsprivate(bp, NULL);
    // FIXME: copmlete bpriv destruction
    // need to iterate through bdeps and clear fsprivates.

    // MARK: this is for softdeps. It call io_deallocate, which is mapped to softdep_deallocate_dependencies.
    if (!LIST_EMPTY(&bpriv->b_dep))
        buf_deallocate(bp);
}

void brelse_callback(buf_t bp, void *arg)
{
    log_debug("buf_filter() called!!!");
    bufpriv_detach_free(bp);
}

void biodone_callback(buf_t bp, void *arg)
{
    int bflags_offset = 0;
    int *b_flags;
    
    // buf_setfilter() is unfortunately a private KPI, so we hack our way to B_FILTER
    // b_timestamp didn't exist until darwin 17.
    if (version_major >= 9 && version_major <= 16){
        bflags_offset = 56;
    } else if (version_major >= 17){
        bflags_offset = 76; // and 'struct timeval b_timestamp' was born.
    } else {
        panic("Cannot set B_FILTER in this version of XNU, (Darwin %d.%d)", version_major, version_minor);
    }
    
    // Note: setting callback after B_FILTER will panic.
    buf_setcallback(bp, brelse_callback, arg);
    
    b_flags = (int*)(((char*)bp)+56);
    OSBitOrAtomic(B_FILTER, b_flags);
        
    // TODO: maybe do some checks on the status of the buffer with regards to b_deps.
}


/*
 * We may have to use buf_bread() isntead of buf_breadn().
 * This gaurantees that any buffer from cache already has fs_private data.
 * However, using breadn strips that guarantee, leaving us a choice between
 * perfomance or simplicity. There must be somthing in between, until then call bread()
 *
 * TODO: breadn() doesn't give us an opportunity to setup any r-ahead buffers
 * that it inserts into cache...
 * leaving us with the option to use bread() or track every buffer with fs_private data.
 * using bread gives guarateed defined behavior when we call buf_fromcache(), but using breadn doesn't.
 */
int
breadn_flags(struct vnode *vp, daddr64_t lblkno, daddr64_t dblkno, int size,
             daddr64_t *rablkno, int *rabsize, int cnt,
             struct ucred *cred, int flags,
             ckhashfunc_t ckhashfunc, struct buf **bpp)
{
    struct buf *bp;
//    struct bufpriv *bpriv;
    int error = 0;
    
    /*
     * buf_*_bread funcs will never deadlock because the pass a 0 slp flag,
     * indicating to getblk that they're not willing to deal with EWOULDBLOCK,
     * or that they won't settle for null on a BL_BUSY buffer.
     * getblk just keeps retrying for that buffer until it's unbusy.
     */
#if 0
    if (flags & B_NOWAIT)
        slpflag = PCATCH;
#endif
    
    /*
     * Attempt to initiate asynchronous I/O on read-ahead blocks.
     * do_breadn_for_type() calls buf_biowait() for us.
     * However, make sure we call it only if pointers aren't null
     */
    if (rablkno && rabsize){
        error = buf_breadn(vp, lblkno, size, rablkno, rabsize, cnt, cred, &bp);
    } else {
        error = buf_bread(vp, lblkno, size, cred, &bp);
    }
    
    // if there's an error,
    if (error != 0) {
        // relse buffer if it error was in biowait()
        if (*bpp) {
            buf_brelse(bp);
            *bpp = NULL;
        }
        // then take the next highway exit
        goto out;
    }
    
    /*
     * We successfully read a buffer.
     * Allocate our fs_private data and check hash if requested.
     */
    
    // allocate bufpriv structure if this is a newly read buffer.
//    bpriv = bufpriv_create(bp);
    
    // this sets the filter for us
//    buf_setcallback(bp, biodone_callback, NULL);

    /* Read is complete, so check if we should ckhash */
    if (ckhashfunc != NULL)
        ckhashfunc(bp);
    
    *bpp = bp;
    
    ASSERT(lblkno == buf_lblkno(bp),
            ("getblkx returned buffer for lblkno %lld instead of lblkno %lld",
             buf_lblkno(bp), (daddr64_t)lblkno));
    
out:
    if (flags && error != 0)
        error = (GB_CVTENXIO);
    return (error);
}

int
meta_bread_flags(struct vnode *vp, daddr64_t lblkno, int size,
                 struct ucred *cred, int flags,
                 ckhashfunc_t ckhashfunc, struct buf **bpp)
{
    struct buf *bp;
//    struct bufpriv *bpriv;
    int error = 0;
    
    /*
     * Attempt to initiate asynchronous I/O on read-ahead blocks.
     */
    error = buf_meta_bread(vp, lblkno, size, cred, &bp);
    
    // if there's an error...
    if (error != 0) {
        // release buffer if it error was in biowait()
        if (*bpp) {
            buf_brelse(*bpp);
            *bpp = NULL;
        }
        // then take the next highway exit
        goto out;
    }
    
    /*
     * We successfully read a buffer.
     * Allocate our fs_private data and check hash if requested.
     */
    
    // allocate bufpriv structure if this is a newly read buffer.
//    bpriv = bufpriv_create(bp);
    
    // this sets the filter for us
//    buf_setcallback(bp, biodone_callback, NULL);

    /* Read is complete, so check if we should ckhash */
    if (ckhashfunc != NULL)
        ckhashfunc(bp);
    
    *bpp = bp;
    
    ASSERT(lblkno == buf_lblkno(bp),
            ("getblkx returned buffer for lblkno %lld instead of lblkno %lld",
             buf_lblkno(bp), (daddr64_t)lblkno));
    
out:
    if (flags && error != 0)
        error = (GB_CVTENXIO);
    return (error);
}

bool
buf_checked_hash(buf_t bp)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    if (bpriv == NULL) trace_return (false);
    uint64_t cflags = bpriv->b_cflags;
    return (cflags & BC_CKHASH) != 0;
}

int
buf_xflags(buf_t bp)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    uint64_t xflags = bpriv->b_xflags;
    return (xflags & BX_XFLAGMASK);
}

int
buf_vflags(buf_t bp)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    uint64_t vflags = bpriv->b_vflags;
    return (vflags & BX_VFLAGMASK);
}

void
buf_setxflags(buf_t bp, int flags)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    bpriv->b_vflags |= (flags & BX_XFLAGMASK);
}

void
buf_setvflags(buf_t bp, int flags)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    bpriv->b_vflags |= (flags & BX_VFLAGMASK);
}

void
buf_clearxflags(buf_t bp, int flags)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    bpriv->b_xflags &= ~(flags & BX_XFLAGMASK);
}

void
buf_clearvflags(buf_t bp, int flags)
{
    struct bufpriv *bpriv = buf_fsprivate(bp);
    bpriv->b_vflags &= ~(flags & BX_VFLAGMASK);
}

buf_t
incore(struct vnode *vp, daddr64_t bn)
{
    struct vfsstatfs *stat;
    struct buf *bp;
    
    stat = vfs_statfs(vnode_mount(vp));
    /*
     * we don't need to worry about buf_deps here because
     * ONLY_VALID specifies that we want existing in-cache buffers.
     */
    bp = buf_getblk(vp, bn, (int)stat->f_iosize, 0, 0, BLK_META | BLK_ONLYVALID);
    
    return (bp && buf_valid(bp)) ? bp : NULL;
}

buf_t getblk(vnode_t vp, daddr64_t blkno, int size, int slpflag, int slptimeo, int operation)
{
    buf_t bp = buf_getblk(vp, blkno, size, slpflag, slptimeo, operation);    
    // allocate b_fsprivate data and set the iodone callback to
    // biodone_callback().
    if (bp != NULL && (operation & (BLK_META | BLK_WRITE))){
        bufpriv_create(bp);
        // this sets the filter for us
        buf_setcallback(bp, biodone_callback, NULL);
    }
    
    return bp;
}
