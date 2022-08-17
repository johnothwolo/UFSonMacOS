/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993, 1995
 *    The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
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
 *    @(#)ufs_ihash.c    8.7 (Berkeley) 5/17/95
 * $FreeBSD: ufs_ihash.c,v 1.36 2002/10/14 03:20:34 mckusick Exp $
 */
 
static const char whatid[] __attribute__ ((unused)) =
"@(#) $Id: ufs_ihash.c,v 1.14 2006/08/22 00:30:19 bbergstrand Exp $";

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/types.h>

#include <freebsd/compat/compat.h>

#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>

char *u_basename(const char *path);

/*
 * Structures associated with inode caching.
 */
static LIST_HEAD(ihashhead, inode) *ihashtbl;
static u_long    ihash;        /* size of hash table - 1 */

#define INOHASH(device, inum)    (&ihashtbl[(minor(device) + (inum)) & ihash])

static inline void hlock(struct ufsmount *ump) {
    lck_mtx_lock((ump)->um_ihash_lock);
}

static inline void  hulock(struct ufsmount *ump){
    lck_mtx_unlock((ump)->um_ihash_lock);
}

static inline void  _hashdestroy(struct ihashhead *tbl, int type, u_long cnt) {
    FREE(tbl, type);
}

/*
 * Initialize inode hash table.
 */
void ufs_hash_init(){
    KASSERT(ihashtbl == NULL, ("ufs_ihashinit called twice for KEXT"));
    ihashtbl = hashinit(desiredvnodes, M_UFSMNT, &ihash);
}

/*
 * Destroy the inode hash table.
 */
void ufs_hash_uninit(){
    _hashdestroy(ihashtbl, M_UFSMNT, ihash);
    ihashtbl = NULL;
}

void
hashdestroy(void *tbl, int type, __unused size_t sz)
{
    FREE(tbl, type);
}

/*
 * Use the device/inum pair to find the incore inode, and return a pointer
 * to it. If it is in core, return it, even if it is locked.
 */
vnode_t ufs_hash_lookup(struct ufsmount *ump, dev_t dev, ino64_t inum){
    struct inode *ip;

    hlock(ump);
    LIST_FOREACH(ip, INOHASH(dev, inum), i_hash){
        if (inum == ip->i_number && dev == ip->i_ump->um_dev)
            break;
    }
    hulock(ump);

    if (ip)
        return (ITOV(ip));
    return (NULLVP);
}

/*
 * Use the device/inum pair to find the incore inode, and return a pointer
 * to it. If it is in core, but locked, wait for it.
 * We're not thread safe rn.
 */
int ufs_hash_get(struct ufsmount *ump, ino64_t inum, int flags, vnode_t *vpp)
{
    struct inode *ip;
    vnode_t vp;
    uint32_t vid;
    dev_t dev = ump->um_dev;
    int error = 0; // if we return 0 and *vpp == NULL, vget continues.
    
    *vpp = NULL;

loop:
    hlock(ump);
    LIST_FOREACH(ip, INOHASH(dev, inum), i_hash) {
        if (inum == ip->i_number && dev == ip->i_ump->um_dev) {
            if (ISSET(ip->i_lflags, INL_ALLOC | INL_TRANSIT)) {
                printf("EXT2-fs DEBUG %p (%s, %d): %s(): sending thread to sleep\n", current_thread(), u_basename(__FILE__), __LINE__, __FUNCTION__);
                /*
                 * inode is getting created or reclaimed wait till
                 * the operation is complete and restart lookup
                 */
                SET(ip->i_lflags, INL_WAIT_ALLOC | INL_WAIT_TRANSIT);
                // the lock is dropped before the thread sleeps and isn't reacquired
#if 0
                lck_rw_sleep(ip->i_lock, LCK_SLEEP_UNLOCK, (event_t)ip, THREAD_ABORTSAFE);
#else
                msleep((caddr_t)ip, ump->um_ihash_lock, PINOD | PDROP, "ufs_hash_get", 0);
#endif
                goto loop;
            }
            
            hulock(ump);

            // If we are no longer on the hash chain, init failed
            if (!error && 0 == (ip->i_lflags & INL_HASHED))
                error = EIO;
            // for EIO
            if (error)
                trace_return (error);
            
            vp = ITOV(ip);
            vid = vnode_vid(vp);
            
            // increases iocount and makes sure the vnode hasn't been discarded
            // also interrupts the vnode stop a vnode being "drained"
            error = vnode_getwithvid(vp, vid);
            if (error == ENOENT)
                goto loop; // vnode is dead and has changed identity
            if (error)
                trace_return (error);
            if (ip != VTOI(vp))
                goto loop;

            *vpp = vp;
            trace_return (0);
        }
    }
    hulock(ump);
    return (0);
}

/*
 * Insert the inode into the hash table, and return it locked.
 */
void ufs_hash_insert(struct inode *ip)
{
    struct ihashhead *ipp;
    
    hlock(ip->i_ump);
    ipp = INOHASH(ip->i_ump->um_dev, ip->i_number);
    LIST_INSERT_HEAD(ipp, ip, i_hash);
    ip->i_lflags |= INL_HASHED;
    hulock(ip->i_ump);
}

/*
 * Remove the inode from the hash table.
 */
void ufs_hash_remove(struct inode *ip)
{
    hlock(ip->i_ump);
    if (ip->i_lflags & INL_HASHED) {
        ip->i_lflags &= ~INL_HASHED;
        LIST_REMOVE(ip, i_hash);
    }
    hulock(ip->i_ump);
}
