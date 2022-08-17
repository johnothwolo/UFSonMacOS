//
//  ext2_ialloc_critical.cpp
//  extfs
//
//  Created by John Othwolo on 7/8/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//
// This makes sure inode duplicates aren't created in ext2_vget() & ext2_valloc()
// But with granular control of which threads wait for what inode.
// Only threads that want indoe X will wait for inode X, instead of every thread waiting in line for
// inode X to finish allocating. Performance increase might be negligible, but why not :).

#ifndef MACH_ASSERT
#define MACH_ASSERT 1
#endif

#include <sys/systm.h>
#include <sys/lock.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSArray.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>

#include <ufs/ufs/inode.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ufs/ufs_extern.h>



extern lck_grp_t *ffs_lock_group;

struct Node;
typedef Node* NodeRef;
typedef OSArray* OSArrayRef;

struct Node : public OSObject {
    OSDeclareDefaultStructors(Node)
    
public:
    void initWithIno(int64_t ino){
        this->ino = ino;
        this->waiters = false;
    }
    
    static NodeRef withIno(int64_t ino){
        NodeRef node = new Node;
        node->initWithIno(ino);
        return node;
    }
    
    void free() override {
        OSObject::free();
    }
    
public:
    ino64_t ino;
    int waiters;
};

OSDefineMetaClassAndStructors(Node, OSObject)

static bool ino_exists(OSArrayRef list, ino64_t ino, NodeRef *nodep, int *index){
    NodeRef node;
    bool exists = false;
    
    for (int i = 0; i < list->getCount(); i++) {
        node = (NodeRef) list->getObject(i);
        
        if(node == nullptr) continue;
        
        if (node->ino == ino){
            exists = true;
            if (nodep) *nodep = node;
            if (index) *index = i;
            break;
        }
    }
    return exists;
}

static void inline ino_insert(OSArrayRef list, ino64_t ino){
    NodeRef node;
    
    node = Node::withIno(ino);
    node->ino = ino;
    node->waiters = 0;
    list->setObject(node); // set object handles the array's expansion.
}

static void inline ino_remove(OSArrayRef list, ino64_t ino){
    NodeRef node;
    int index = -1; // we set to -1 just to ensure the sanity of the index value if node exists.
    
    if (ino_exists(list, ino, &node, &index)){
        assert(index >= 0 && node != nullptr);
        list->removeObject(index);
        node->release();
    }
}

#pragma mark -

/*
 * Structures associated with inode caching.
 */
struct ialloc_critical {
    OSArrayRef ic_list;
    lck_mtx_t *ic_lock;
};

static inline void hlock(struct ialloc_critical *crit) {
    lck_mtx_lock(crit->ic_lock);
}

static inline void  hunlock(struct ialloc_critical *crit){
    lck_mtx_unlock(crit->ic_lock);
}

struct ialloc_critical* ialloc_critical_new(void)
{
    struct ialloc_critical *crit = new ialloc_critical;
    crit->ic_lock = lck_mtx_alloc_init(ffs_lock_group, LCK_ATTR_NULL);
    crit->ic_list = OSArray::withCapacity(USHRT_MAX);
    return crit;
}

void ialloc_critical_free(struct ialloc_critical *crit)
{
    OSArrayRef list;
    NodeRef node;
    if (crit == NULL) return;
    
    hlock(crit);
    list = crit->ic_list;
    for (int i = 0; i < list->getCount(); i++) {
        node = (NodeRef) list->getObject(i);
        if(node == nullptr) continue;
        list->removeObject(i);
        node->free();
    }
    hunlock(crit);
    
    lck_mtx_destroy(crit->ic_lock, ffs_lock_group);
    lck_mtx_free(crit->ic_lock, ffs_lock_group);
    list->free();
    delete crit;
}

void ialloc_critical_wait(struct ialloc_critical* crit, ino64_t ino)
{
    NodeRef node;
    hlock(crit);
    if (ino_exists(crit->ic_list, ino, &node, nullptr)){
        node->waiters += 1;
        hunlock(crit);
        msleep(node, NULL, PVFS | PCATCH, "ialloc_critical_wait", 0); // wait
    } else{
        hunlock(crit);
    }
}

bool ialloc_is_critical(struct ialloc_critical* crit, ino64_t ino)
{
    bool exists = false;
    hlock(crit);
    if (ino_exists(crit->ic_list, ino, nullptr, nullptr)){
        exists = true;
    }
    hunlock(crit);
    return exists;
}

void ialloc_critical_enter(struct ialloc_critical* crit, ino64_t ino)
{
    hlock(crit);
    assert(ino_exists(crit->ic_list, ino, nullptr, nullptr) == false);
    ino_insert(crit->ic_list, ino);
    hunlock(crit);
}

/* leave critical stage and wakeup waiters if they exist */
void ialloc_critical_leave(struct ialloc_critical* crit, ino64_t ino)
{
    NodeRef node;
    hlock(crit);
    assert(ino_exists(crit->ic_list, ino, &node, nullptr) == true);
    if (node->waiters > 0){
        wakeup(node);
    }
    ino_remove(crit->ic_list, ino);
    hunlock(crit);
}
