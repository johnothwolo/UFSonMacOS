/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2005 Poul-Henning Kamp
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/lock.h>
#include <sys/vnode.h>

#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>


#include <freebsd/sys/compat.h>

static LIST_HEAD(vfs_hash_head, inode)    *vfs_hash_tbl;
static LIST_HEAD(,inode)        vfs_hash_side;
static u_long                vfs_hash_mask;
static lck_rw_t vfs_hash_lock;
static inline void  _hashdestroy(struct vfs_hash_head *tbl, int type, u_long cnt)
{
    FREE(tbl, type);
}


void
vfs_hashinit(void)
{

    vfs_hash_tbl = hashinit(desiredvnodes, M_TEMP, &vfs_hash_mask);
    lck_rw_init(&vfs_hash_lock, LCK_GRP_NULL, LCK_ATTR_NULL);
    LIST_INIT(&vfs_hash_side);
}

void
vfs_hashuninit(void)
{

    _hashdestroy(vfs_hash_tbl, M_TEMP, vfs_hash_mask);
    lck_rw_destroy(&vfs_hash_lock, LCK_GRP_NULL);
}

static struct vfs_hash_head *
vfs_hash_bucket(const struct ufsmount *ump, u_int hash)
{
    return (&vfs_hash_tbl[(hash + (uintptr_t)ump->um_mountp) & vfs_hash_mask]);
}

int
vfs_hash_get(const struct mount *mp, u_int hash, int flags, struct thread *td,
             struct vnode **vpp, vfs_hash_cmp_t *fn, void *arg)
{
    struct inode *ip = NULL;
    int error;

    while (1) {
        lck_rw_lock_shared(&vfs_hash_lock);
        LIST_FOREACH(ip, vfs_hash_bucket(ITOUMP(ip), hash), i_hashlist) {
            if (ip->i_hash != hash)
                continue;
            if (vnode_mount(ITOV(ip)) != mp)
                continue;
            if (fn != NULL && fn(ITOV(ip), arg))
                continue;

            lck_rw_unlock_shared(&vfs_hash_lock);
            error = vnode_get(ITOV(ip));
            if (error)
                return (error);
            *vpp = ITOV(ip);
            return (0);
        }
        if (ip == NULL) {
            lck_rw_unlock_shared(&vfs_hash_lock);
            *vpp = NULL;
            return (0);
        }
    }
}

void
vfs_hash_remove(struct vnode *vp)
{
    lck_rw_lock_exclusive(&vfs_hash_lock);
    LIST_REMOVE(VTOI(vp), i_hashlist);
    lck_rw_unlock_exclusive(&vfs_hash_lock);
}

int
vfs_hash_insert(struct vnode *vp, u_int hash, int flags, struct thread *td,
    struct vnode **vpp, vfs_hash_cmp_t *fn, void *arg)
{
    struct inode *ip, *ip2;
    int error;

    ip2 = NULL;
    *vpp = NULL;
    ip = VTOI(vp);
    
    while (1) {
        lck_rw_lock_exclusive(&vfs_hash_lock);
        LIST_FOREACH(ip2, vfs_hash_bucket(ITOUMP(ip2), hash), i_hashlist) {
            if (ip2->i_hash != hash)
                continue;
            if (ITOUMP(ip2)->um_mountp != vnode_mount(vp))
                continue;
            if (fn != NULL && fn(ITOV(ip2), arg))
                continue;
            
            lck_rw_unlock_exclusive(&vfs_hash_lock);
            error = vnode_get(ITOV(ip2));
            lck_rw_lock_exclusive(&vfs_hash_lock);
            LIST_INSERT_HEAD(&vfs_hash_side, ip, i_hashlist);
            lck_rw_unlock_exclusive(&vfs_hash_lock);
            vn_revoke(vp, 0, vfs_context_current());
            vnode_put(vp);
            if (!error)
                *vpp = ITOV(ip2);
            return (error);
        }
        if (ip2 == NULL)
            break;
    }
    ip2->i_hash = hash;
    LIST_INSERT_HEAD(vfs_hash_bucket(ITOUMP(ip), hash), ip, i_hashlist);
    lck_rw_unlock_exclusive(&vfs_hash_lock);
    return (0);
}

void
vfs_hash_rehash(struct vnode *vp, u_int hash)
{
    struct inode *ip = VTOI(vp);
    lck_rw_lock_exclusive(&vfs_hash_lock);
    LIST_REMOVE(ip, i_hashlist);
    LIST_INSERT_HEAD(vfs_hash_bucket(ITOUMP(ip), hash), ip, i_hashlist);
    ip->i_hash = hash;
    lck_rw_unlock_exclusive(&vfs_hash_lock);
}
