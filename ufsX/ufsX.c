//
//  ufsX.c
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <mach/mach_types.h>



// TODO: init procedures
void softdep_lock_init(void);
void softdep_lock_uninit(void);
void vfs_hashinit(void);
void vfs_hashuninit(void);
void fbsd_vops_init(void);

kern_return_t ufsX_start(kmod_info_t * ki, void *d);
kern_return_t ufsX_stop(kmod_info_t *ki, void *d);

kern_return_t ufsX_start(kmod_info_t * ki, void *d)
{
    return KERN_SUCCESS;
}

kern_return_t ufsX_stop(kmod_info_t *ki, void *d)
{
    return KERN_SUCCESS;
}
