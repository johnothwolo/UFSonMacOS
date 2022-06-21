//
//  fbsd_devfsprivate.c
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/ioccom.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/kauth.h>

#include <miscfs/devfs/devfs.h>

#include <freebsd/sys/compat.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

struct cdev_privdata {
    dev_t                       cdp_dev;
    void                       *cdpd_data;
    void                      (*cdpd_dtr)(void *);
    LIST_ENTRY(cdev_privdata)   cdpd_entry;
};

static  UFS_MALLOC_DEFINE(M_VFS_HASH, "vfs_hash", "VFS hash table");
static  LIST_HEAD(hash_head, cdev_privdata)  *devfspriv_hash_tbl;
static  lck_mtx_t                             devfspriv_hash_lock;
static  u_long                                devfspriv_hash_size;        /* size of hash table - 1 */
#define DEVHASH(device)                     (&devfspriv_hash_tbl[(minor(device)) & devfspriv_hash_size])

static inline void hlock(void) {
    lck_mtx_lock(&devfspriv_hash_lock);
}

static inline void  hunlock(void){
    lck_mtx_unlock(&devfspriv_hash_lock);
}

/*
 * Initialize inode hash table.
 */
void devfspriv_hash_init(void)
{
    KASSERT(devfspriv_hash_tbl == NULL, ("devfspriv_hash_init called twice for KEXT"));
    devfspriv_hash_tbl = hashinit(desiredvnodes, M_TEMP, &devfspriv_hash_size);
    // set all slots to point to zero
    memset(devfspriv_hash_tbl, -1, devfspriv_hash_size);
}

/*
 * Destroy the inode hash table.
 */
void devfspriv_hash_destroy(void)
{
    _FREE(devfspriv_hash_tbl, M_TEMP);
    devfspriv_hash_tbl = NULL;
}

static struct cdev_privdata*
get_priv(dev_t dev)
{
    struct cdev_privdata *priv_data, *ret = NULL;
    
    hlock();
    LIST_FOREACH(priv_data, devfspriv_hash_tbl, cdpd_entry) {
        if(priv_data->cdp_dev == dev){
            ret = priv_data;
            break;
        }
    }
    hunlock();
    return ret;
}

int devfs_get_cdevpriv(dev_t dev, void **datap)
{
    struct cdev_privdata *priv_data = get_priv(dev);

    if (priv_data == NULL)
        return (ENODATA);
    
    *datap = priv_data->cdpd_data;
    return (0);
}

int devfs_set_cdevpriv(dev_t dev, void *priv, d_priv_dtor_t *dtr)
{
    struct hash_head *slot;
    struct cdev_privdata *priv_data;
    
    priv_data = _MALLOC(sizeof(struct cdev_privdata), M_TEMP, M_ZERO);
    slot = DEVHASH(dev);
    priv_data->cdpd_data = priv;
    priv_data->cdpd_dtr = dtr;
    
    if (get_priv(dev) != NULL){
        // if we successfully find data, return EBUSY.
        // we can only call ffs_suspend once
        return EBUSY;
    }
    
    // else insert the the data
    hlock();
    LIST_INSERT_HEAD(slot, priv_data, cdpd_entry);
    hunlock();
    return 0;
}

void devfs_clear_cdevpriv(dev_t dev)
{
    struct cdev_privdata *priv_data = get_priv(dev);
    
    // if priv_data doesn't exist, return
    if (priv_data == NULL)
        return;
    
    // else, delete the entry
    hlock();
    LIST_REMOVE(priv_data, cdpd_entry);
    hunlock();
    priv_data->cdpd_dtr(priv_data->cdpd_data);
    _FREE(priv_data, sizeof(struct cdev_privdata));
}

