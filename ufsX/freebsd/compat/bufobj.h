/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2004 Poul-Henning Kamp
 * All rights reserved.
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

/*
 * Architectural notes:
 *
 * bufobj is a new object which is what buffers hang from in the buffer
 * cache.
 *
 * This used to be vnodes, but we need non-vnode code to be able
 * to use the buffer cache as well, specifically geom classes like gbde,
 * raid3 and raid5.
 *
 * All vnodes will contain a bufobj initially, but down the road we may
 * want to only allocate bufobjs when they are needed.  There could be a
 * large number of vnodes in the system which wouldn't need a bufobj during
 * their lifetime.
 *
 * The exact relationship to the vmobject is not determined at this point,
 * it may in fact be that we find them to be two sides of the same object
 * once things starts to crystalize.
 */

#ifndef _SYS_BUFOBJ_H_
#define _SYS_BUFOBJ_H_

#if defined(_KERNEL) || defined(_KVM_VNODE)

#include <sys/lock.h>
#include <sys/queue.h>
#include <freebsd/compat/_pctrie.h>

struct bufobj;
struct buf_ops;

extern struct buf_ops buf_ops_bio;

typedef void bo_strategy_t(struct bufobj *, struct buf *);
typedef int bo_write_t(struct buf *);
typedef int bo_sync_t(struct bufobj *, int waitfor);
typedef void bo_bdflush_t(struct bufobj *, struct buf *);

struct buf_ops {
    const char    *bop_name;
    bo_write_t    *bop_write;
    bo_strategy_t    *bop_strategy;
    bo_sync_t    *bop_sync;
    bo_bdflush_t    *bop_bdflush;
};

/*
 * Locking notes:
 * 'S' is sync_mtx
 * 'v' is the vnode lock which embeds the bufobj.
 * '-' Constant and unchanging after initialization.
 */
struct bufobj {
    lck_rw_t          *bo_lock;    /* Lock which protects "i" things */
    struct buf_ops    *bo_ops;     /* - Buffer operations */
    int                bo_bsize;   /* - Block size for i/o */
};

static inline void BO_STRATEGY(struct bufobj *bufobj, struct buf *bp){
    bufobj->bo_ops->bop_strategy(bufobj, bp);
}

static inline int BO_WRITE(struct bufobj *bufobj, struct buf *bp){
    return bufobj->bo_ops->bop_write(bp);
}

static inline int BO_SYNC(struct bufobj *bufobj, int waitfor){
    return bufobj->bo_ops->bop_sync(bufobj, waitfor);
}

static inline void BO_BDFLUSH(struct bufobj *bufobj, struct buf *bp){
    bufobj->bo_ops->bop_bdflush(bufobj, bp);
}

/*
 * XXX BO_ONWORKLST could be replaced with a check for NULL list elements
 * in v_synclist.
 */
#define    BO_ONWORKLST    (1 << 0)    /* On syncer work-list */
#define    BO_WWAIT    (1 << 1)    /* Wait for output to complete */
#define    BO_DEAD        (1 << 2)    /* Dead; only with INVARIANTS */
#define    BO_NOBUFS    (1 << 3)    /* No bufs allowed */

#define    BO_LOCKPTR(bo)               ((bo)->bo_lock)
#define    BO_LOCK(bo)                  lck_rw_lock_exclusive(BO_LOCKPTR((bo)))
#define    BO_UNLOCK(bo)                lck_rw_unlock_exclusive(BO_LOCKPTR((bo)))
#define    BO_RLOCK(bo)                 lck_rw_lock_shared(BO_LOCKPTR((bo)))
#define    BO_RUNLOCK(bo)               lck_rw_unlock_shared(BO_LOCKPTR((bo)))
#define    ASSERT_BO_WLOCKED(bo)        lck_rw_assert(BO_LOCKPTR((bo)), RA_WLOCKED)
#define    ASSERT_BO_LOCKED(bo)         lck_rw_assert(BO_LOCKPTR((bo)), RA_LOCKED)
#define    ASSERT_BO_UNLOCKED(bo)       lck_rw_assert(BO_LOCKPTR((bo)), RA_UNLOCKED)

void bufobj_init(struct bufobj *bo, void *priv);
void bufobj_wdrop(struct bufobj *bo);
void bufobj_wref(struct bufobj *bo);
void bufobj_wrefl(struct bufobj *bo);
int bufobj_invalbuf(struct bufobj *bo, int flags, int slpflag, int slptimeo);
int bufobj_wwait(struct bufobj *bo, int slpflag, int timeo);
int bufsync(struct bufobj *bo, int waitfor);
void bufbdflush(struct bufobj *bo, struct buf *bp);

#endif /* defined(_KERNEL) || defined(_KVM_VNODE) */
#endif /* _SYS_BUFOBJ_H_ */
