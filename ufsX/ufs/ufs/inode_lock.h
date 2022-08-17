//
//  inode_lock.h
//  ufsX
//
//  Created by John Othwolo on 7/27/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef inode_lock_h
#define inode_lock_h

#include <sys/cdefs.h>
#include <sys/lock.h>
#include <sys/queue.h>

#include <libkern/OSAtomic.h>
#include <freebsd/compat/vnode.h>

#include <ufs/ufs/dinode.h>
#include <ufs/ufs/quota.h>

/* Lock types */
typedef enum {
    UFS_LOCK_SHARED     = LCK_RW_TYPE_SHARED,
    UFS_LOCK_EXCLUSIVE  = LCK_RW_TYPE_EXCLUSIVE,
    UFS_LOCK_TYPE_MASK  = UFS_LOCK_SHARED | UFS_LOCK_EXCLUSIVE,
    
    UFS_LOCK_INTERLOCK  = 0x000100,
    UFS_LOCK_NOWAIT     = 0x000200,
    UFS_LOCK_RETRY      = 0x000400,
    UFS_LOCK_SLEEPFAIL  = 0x000800,

    UFS_LOCK_DOWNGRADE  = 0x010000,
    UFS_LOCK_UPGRADE    = 0x400000,
    UFS_LOCK_TRYUPGRADE = 0x800000,
} lock_type_t;

struct inode_lock;
static thread_t UFS_THREAD_NULL  = (thread_t)0;

struct inode_lock *inode_lock_new(void);
void inode_lock_free(struct inode *ip);
int  inode_lock_owned(struct inode *ip);
bool inode_lock_lock(struct inode *ip, lock_type_t locktype);
bool inode_lock_upgrade(struct inode *ip);
void inode_lock_downgrade(struct inode *ip);
int  inode_lock_lockpair(struct inode *ip1, struct inode *ip2, lock_type_t locktype);
void inode_lock_unlock(struct inode *ip);
void inode_lock_unlockpair(struct inode *ip1, struct inode *ip2);
void inode_wakeup(struct inode *ip, int flag, int clearflag); // call with exclusive lock held


/* Locking */
#define ixlock(ip)      ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"acquiring exclusive lock");\
    inode_lock_lock((ip), UFS_LOCK_EXCLUSIVE);\
})

#define ilockpair(ip0, ip1, type)      ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"acquiring pair of exclusive locks");\
    inode_lock_lockpair((ip0), (ip1), type);\
})

#define iunlockpair(ip0, ip1)      ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"acquiring pair of exclusive locks");\
    inode_lock_unlockpair((ip0), (ip1));\
})

#define islock(ip)      ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"acquiring shared lock");\
    inode_lock_lock((ip), UFS_LOCK_SHARED);\
})

#define iupgradelock(ip)   ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"upgrading lock");\
    inode_lock_upgrade((ip));\
})
#define idowngradelock(ip)   ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"downgrading lock");\
    inode_lock_downgrade((ip));\
})
#define iunlock(ip)     ({\
    __log_debug(current_thread(), __FILE__, __LINE__, __func__,"releasing lock"); \
    inode_lock_unlock((ip));\
})

#endif /* inode_lock_h */
