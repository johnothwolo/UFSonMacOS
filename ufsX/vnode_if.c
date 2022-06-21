//
//  lock.c
//  ufsX
//
//  Created by John Othwolo on 6/20/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <kern/kalloc.h>
#include <sys/mount.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>

#include <freebsd/sys/compat.h>

#include <ufs/ufsX_vnops.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>


enum { NULLTHREAD = -1 };
typedef uint64_t tid_t;

struct lock {
    // we set is to double the max xnu config value
    tid_t readers[10240];  // threads with a readlock
    int flags; // for stuff like lk_recurse
    int recurse; //TODO: maybe use readers as a hashtable for recurse instead.
    tid_t writer; // thread that excl owns the lock
    lck_rw_t rw;
};

#pragma mark -

#pragma mark VNOP_ISLOCKED

int VNOP_ISLOCKED_APV(VOPFUNC *vop __unused, struct vnop_islocked_args *ap)
{
    struct lock *lp = VTOI(ap->a_vp)->i_lock;
    tid_t tid = thread_tid(current_thread());
    
    ufs_debug("enter");
    
    if (lp->writer != NULLTHREAD){
        if (lp->writer == tid){
            return LK_EXCLUSIVE;
        } else {
            return LK_EXCLOTHER;
        }
    } else {
        return 0;
    }
}

int VOP_ISLOCKED(struct vnode *vp)
{
    struct vnop_islocked_args a;
    a.a_vp = vp;
    return (VOP_ISLOCKED_APV(NULL, &a));
}


#pragma mark -

#pragma mark VNOP_LOCK

int VNOP_LOCK1_APV(VOPFUNC *vop __unused, struct vnop_lock1_args *ap)
{
    ufs_debug("VNOP_LOCK1_APV called from %s:%d", ap->a_file, ap->a_line);
    
    return std_lock(VTOI(ap->a_vp), ap->a_flags, ap->a_file, ap->a_line);
}

int VNOP_LOCK1(struct vnode *vp, int flags, const char *file, int line)
{
    struct vnop_lock1_args a;
    a.a_vp = vp;
    a.a_flags = flags;
    a.a_file = file;
    a.a_line = line;
    return (VNOP_LOCK1_APV(NULL, &a));
}

#pragma mark -

#pragma mark VNOP_UNLOCK

int VNOP_UNLOCK_APV(VOPFUNC *vop __unused, struct vnop_unlock_args *ap)
{
    ufs_debug("VNOP_UNLOCK_APV called from %s:%d", ap->a_file, ap->a_line);
    
    std_unlock(VTOI(ap->a_vp), 0, ap->a_file, ap->a_line);
    
    return 0;
}

int VNOP_UNLOCK1(struct vnode *vp, const char *file, int line)
{
    struct vnop_unlock_args a;
    a.a_vp = vp;
    a.a_file = file;
    a.a_line = line;
    return (VNOP_UNLOCK_APV(NULL, &a));
    
}

#pragma mark -

#pragma mark LOCKS...

struct lock* std_newlock(void)
{
    struct lock *lp = kalloc(sizeof(struct lock));
    memset(lp, 0, sizeof(struct lock));
    memset(lp->readers, NULLTHREAD, sizeof(lp->readers));
    lck_rw_init(&lp->rw, LCK_GRP_NULL, LCK_ATTR_NULL);
    return lp;
}

void std_destroylock(struct lock *lock)
{
    if (!lock) return;
    // destroy lock
    lck_rw_destroy(&lock->rw, LCK_GRP_NULL);
    kfree(lock, sizeof(struct lock));
}

// NOTE: *_try_lock_* returns true if lock is sucessfully acquired
// NOTE: we retry 3 time if LK_RETRY is specified
int std_lock(struct inode *ip, int flags, const char *file, int line)
{
    struct lock *lp = ip->i_lock;
    int ret = 0, tried_again = 0;
    thread_t curthread = current_thread();
    tid_t tid = thread_tid(curthread);
    
_TryAgain:
    if (lp->writer == tid && (flags & LK_CANRECURSE)){
        lp->recurse++;
    }
    
    // EXCLUSIVE LOCK
    if ((flags & (LK_EXCLUSIVE | LK_DRAIN)) != 0){
        // for DRAIN, just x-lock because the lock is about to be freed.
        if (flags & LK_NOWAIT){
            if ((ret = !lck_rw_try_lock_exclusive(&lp->rw)) != 0)
                goto done; // if we fail, skip everything a goto done
        }
        else lck_rw_lock_exclusive(&lp->rw);
        lp->writer = tid; // make us writer
        lp->readers[tid] = NULLTHREAD; // delete our reading value
    }
    // SHARED LOCK
    else if (flags & LK_SHARED){
        lck_rw_lock_shared(&lp->rw); // lock
        lp->readers[tid] = tid; // register thread
    }
    // LOCK UPGRADE
    else if (flags & LK_UPGRADE){
        if ((flags & LK_NOWAIT)){
            if ((ret = !lck_rw_try_lock_exclusive(&lp->rw)) != 0)
                goto done; // if we fail, skip everything a goto done
        }
        else lck_rw_lock_shared_to_exclusive(&lp->rw);
        lp->writer = tid; // make us writer
        lp->readers[tid] = NULLTHREAD; // delete our reading value
    }
    // LOCK DOWNGRADE
    else if (flags & LK_DOWNGRADE){
        if (lp->writer == tid){
            lp->readers[tid] = NULLTHREAD;
            lp->writer = NULLTHREAD;
            lck_rw_lock_exclusive_to_shared(&lp->rw);
        }
        else if(lp->readers[tid] == NULLTHREAD) // we just panic here
            panic("Trying to upgrade readlock we do not own");
    }
    
done:
    
    if (ret != 0 && tried_again < 3){
        tried_again++;
        goto _TryAgain;
    }
    
    return 0;
}

void std_unlock(struct inode *ip, int flags, const char *file, int line)
{
    struct lock *lp = ip->i_lock;
    tid_t tid = thread_tid(current_thread());
    
    if (lp->writer == tid){
        lck_rw_unlock_exclusive(&lp->rw);
        lp->writer = NULLTHREAD;
    } else{
        lck_rw_unlock_shared(&lp->rw);
        lp->readers[tid] = NULLTHREAD; // unregister reader
    }
}

#pragma mark -

