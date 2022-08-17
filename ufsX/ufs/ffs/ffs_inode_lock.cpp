//
//  ext2_lock.c
//  extfs
//
//  Created by John Othwolo on 7/13/22.
//  Copyright © 2022 jto. All rights reserved.
//

extern "C"{
#ifndef MACH_ASSERT
#define MACH_ASSERT 1
#endif
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/buf.h>

#include <ufs/ufs/inode.h>
#include <ufs/ffs/fs.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
//#include <ufs/ufs/ufs_extattr.h>
//#include <ufs/ffs/ufs_dir.h>
//#include <ufs/ufs/ufs_vnops.h>
}

#include <libkern/libkern.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/OSAtomic.h>

enum {
    LOCK_WAITING_EXCL = 0x10
};

struct inode_lock {
    lck_rw_t           *lock_Impl;          /* underlying system lock for implementation */
    long long           lock_rdcount;       /* number of readers in this lock */
    thread_t            lock_owner;         /* Owner of exclusive lock on the inode */
    int64_t             lock_flags;         /* lock flags */
    OSDictionary        *lock_readers;       /* threads holding a read lock on inode */
};

#pragma mark - inode_lock RwReaders stuff

void tid_string(char *result, long num){
    char str[32] = {0};
    char *p = result, *s = &str[0];
    int n = 0; // digit count
    
    for (long i = num; i > 0 && s; i /= 10, s++, n++) {
        *s = (i % 10) + 0x30; // 0x30 is index of 0 in ascii chart.
    }
    
    for (s--; n > 0; n--, s--, p++) {
        *p = *s;
    }
}

static inline bool has_readlock(struct inode_lock *lock){
    long tid = thread_tid(current_thread());
//    assert_reader_bounds(tid);
//    return lock->lock_readers[tid];
    bool exists;
    char tidString[32] = {0};
    tid_string(tidString, tid);
    OSString *str = OSString::withCStringNoCopy(tidString);
    exists = lock->lock_readers->getObject(str) != nullptr;
    str->free();
    return exists;
}

// set current thread to own readlock
static inline void set_readlock(struct inode_lock *lock){
    long tid = thread_tid(current_thread());
//    assert_reader_bounds(tid);
//    lock->lock_readers[tid] = true;
    char tidString[32] = {0};
    tid_string(tidString, tid);
    OSString *tidStr = OSString::withCString(tidString);
    lock->lock_readers->setObject(tidStr, tidStr);
}

// clear current thread to own readlock
static inline void clear_readlock(struct inode_lock *lock){
    long tid = thread_tid(current_thread());
//    assert_reader_bounds(tid);
//    lock->lock_readers[tid] = false;
    char tidString[32] = {0};
    tid_string(tidString, tid);
    OSString *tidStr = OSString::withCStringNoCopy(tidString), *dictStr;
    dictStr = (OSString *)lock->lock_readers->getObject(tidStr);
    assert(dictStr != nullptr);
    tidStr->free();
    lock->lock_readers->removeObject(dictStr);
}

#pragma mark - Exported functions

struct inode_lock *inode_lock_new(void)
{
    struct inode_lock *lock = (struct inode_lock*)_MALLOC(sizeof(struct inode_lock), M_TEMP, M_ZERO | M_WAITOK);
    if(lock == nullptr) panic("Could not allocate inode lock");
    lock->lock_Impl = lck_rw_alloc_init(ffs_lock_group, LCK_ATTR_NULL);
    lock->lock_readers = OSDictionary::withCapacity(256);
    return lock;
}

void inode_lock_free(struct inode *ip)
{
    lck_rw_destroy(ip->i_lock->lock_Impl, ffs_lock_group);
    ip->i_lock->lock_readers->free();
    _FREE(ip->i_lock, M_TEMP);
}

int inode_lock_owned(struct inode *ip)
{
    struct inode_lock *lock = ip->i_lock;

    if (lock->lock_owner == current_thread()){
        return UFS_LOCK_EXCLUSIVE;
    } else if(has_readlock(lock)){
        return UFS_LOCK_SHARED;
    } else {
        return 0;
    }
}

/*
 * Lock a inode.
 */
bool inode_lock_lock(struct inode *ip, lock_type_t locktype)
{
    struct inode_lock *lock = ip->i_lock;
    bool ret = false;

    if (ISSET(locktype, UFS_LOCK_EXCLUSIVE)) {
        if (ISSET(locktype, UFS_LOCK_NOWAIT)){
            ret = lck_rw_try_lock(lock->lock_Impl, UFS_LOCK_EXCLUSIVE);
        } else {
            lck_rw_lock_exclusive(lock->lock_Impl);
            ret = true;
        }
        lock->lock_owner = current_thread();
        return ret;
    }
    
    if (ISSET(locktype, UFS_LOCK_SHARED)){
        if (ISSET(locktype, UFS_LOCK_NOWAIT)){
            ret = lck_rw_try_lock(lock->lock_Impl, UFS_LOCK_SHARED);
        } else {
            lck_rw_lock_shared(lock->lock_Impl);
            ret = true;
        }
        OSIncrementAtomic64(&lock->lock_rdcount);
        set_readlock(lock);
        return ret;
    }
    panic("Unspecified lock request.");
    __builtin_unreachable();
}

/*
 * Unlock an inode.
 * if a thread aqcuires a shared lock, it should be allowed to release it
 */
void inode_lock_unlock(struct inode *ip)
{
    struct inode_lock *lock = ip->i_lock;
        
    if(lock->lock_owner == current_thread()){
        lock->lock_owner = UFS_THREAD_NULL;
        lck_rw_unlock_exclusive(lock->lock_Impl);
    } else if (lock->lock_rdcount > 0) {
        OSDecrementAtomic64(&lock->lock_rdcount);
        clear_readlock(lock);
        lck_rw_unlock_shared(lock->lock_Impl);
    } else {
        panic("Attempting to unlock Inode lock, not read or write owned!");
        __builtin_unreachable();
    }
    
}

bool inode_lock_upgrade(struct inode *ip)
{
    struct inode_lock *lock = ip->i_lock;

    // lock_rdcount has to be >= 1 before we decrement.
    assert(has_readlock(lock) && lock->lock_rdcount > 0);
    lck_rw_lock_shared_to_exclusive(lock->lock_Impl);
    lock->lock_owner = current_thread();
    OSDecrementAtomic64(&lock->lock_rdcount);
    clear_readlock(lock);
    return true;
}

void inode_lock_downgrade(struct inode *ip){
    struct inode_lock *lock = ip->i_lock;
        
    assert(lock->lock_owner == current_thread());
    lock->lock_owner = UFS_THREAD_NULL;
    OSIncrementAtomic64(&lock->lock_rdcount); // we're now a reader, so increment
    set_readlock(lock);
    lck_rw_lock_exclusive_to_shared(lock->lock_Impl);

}

/*
 * Lock a pair of inodes.
 */
int inode_lock_lockpair(struct inode *ip1, struct inode *ip2, lock_type_t locktype)
{
    struct inode *first, *last;
    struct inode_lock *lock1 = ip1->i_lock, *lock2 = ip2->i_lock;
    thread_t td = current_thread();
    int error;
    
    if (locktype & UFS_LOCK_EXCLUSIVE){
        if (lock2->lock_owner == td || lock1->lock_owner == td)
            panic("%p already owns one inode", td);
    }
    /*
     * If inodes match then just lock one.
     */
    if (ip1 == ip2) {
        return inode_lock_lock(ip1, locktype);
    }

    /*
     * Lock in inode address order.
     */
    if (ip1 < ip2) {
        first = ip1;
        last = ip2;
    } else {
        first = ip2;
        last = ip1;
    }

    if ( (error = inode_lock_lock(first, locktype))) {
        return (error);
    }
    if ( (error = inode_lock_lock(last, locktype))) {
        inode_lock_unlock(first);
        return (error);
    }
    return (0);
}

/*
 * Unlock a pair of inodes.
 */
void inode_lock_unlockpair(struct inode *ip1, struct inode *ip2)
{
    inode_lock_unlock(ip1);
    if (ip2 != ip1)
        inode_lock_unlock(ip2);
}

void inode_wakeup(struct inode *ip, int flags, int clearflag)
{
//    assert(inode_lock_owned(ip) == UFS_LOCK_EXCLUSIVE);
    if (clearflag)
        CLR(ip->i_lflags, clearflag);
    if (ISSET(ip->i_lflags, flags)){
        CLR(ip->i_lflags, flags);
        wakeup(ip);
    }
}
