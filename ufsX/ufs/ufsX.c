//
//  ufsX.c
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <mach/mach_types.h>
#include <mach/mach_time.h>

#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/ucred.h>

#include <libkern/libkern.h> // crc32

#include <freebsd/compat/compat.h>

#include <ufs/ufsX_vnops.h>
#include <ufs/ufsX_vfsops.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ffs/ffs_extern.h>
#include <ufs/ffs/fs.h>

lck_grp_t *ffs_lock_group;
static vfstable_t ufs_tableid;
static struct vnodeopv_desc* vnops[] = {
    &ufsX_vnodeop_opv_desc,
    &ufsX_specop_opv_desc,
    &ufsX_fifoop_opv_desc
};

// TODO: init procedures
void softdep_lock_init(void);
void softdep_lock_uninit(void);

void fbsd_vops_init(void);
// logging
void log_debug_init(void);
void log_debug_uninit(void);

kern_return_t ufsX_start(kmod_info_t * ki, void *d);
kern_return_t ufsX_stop(kmod_info_t *ki, void *d);

kern_return_t ufsX_start(kmod_info_t * ki, void *d)
{
    lck_grp_attr_t *lgattr;
    struct vfs_fsentry fsc = {0};
    int kret;
    
    lgattr = lck_grp_attr_alloc_init();
    if (lgattr){
        lck_grp_attr_setstat(lgattr);
    }
    
    if (NULL == (ffs_lock_group = lck_grp_alloc_init("UFS Filesystem", lgattr))){
        trace_return (KERN_RESOURCE_SHORTAGE);
    }
    
    if (lgattr){
        lck_grp_attr_free(lgattr);
    }
    
    log_debug_init();
    ufs_hash_init();
       
    fsc.vfe_vfsops = &ffs_vfsops;
    fsc.vfe_vopcnt = nitems(vnops);
    fsc.vfe_opvdescs = vnops;
    fsc.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBL64BITREADY;
    strlcpy(fsc.vfe_fsname, UFS_NAME, MIN(MFSNAMELEN, sizeof(UFS_NAME)));
    
    kret = vfs_fsadd(&fsc, &ufs_tableid);
    if (kret != 0) {
        log_debug ("ufsX_start: Failed to register with kernel, error = %d\n", kret);
        lck_grp_free(ffs_lock_group);
        trace_return (KERN_FAILURE);
    }
    
    log_debug ("ufsX_start: Successfully registered FS with kernel, ret = %d\n", kret);
    
    trace_return (KERN_SUCCESS);
}

kern_return_t ufsX_stop(kmod_info_t *ki, void *d)
{
    int error;
     
     /* Unregister fs from the kernel */
     if ((error = vfs_fsremove(ufs_tableid)))
         trace_return (error);
    
    ufs_uninit(NULL);
    log_debug_uninit();
    ufs_hash_uninit();
    lck_grp_free(ffs_lock_group);
    
    return (KERN_SUCCESS);
}
