//
//  ufs_vnops.h
//  ufsX
//
//  Created by John Othwolo on 6/19/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef ufs_vnops_h
#define ufs_vnops_h

struct vnodeopv_entry_desc;


typedef int (*VOPFUNC)              (void *);
typedef int (vnop_open_t)           (struct vnop_open_args *);
typedef int (vnop_close_t)          (struct vnop_close_args *);
typedef int (vnop_create_t)         (struct vnop_create_args *);
typedef int (vnop_remove_t)         (struct vnop_remove_args *);
typedef int (vnop_getattr_t)        (struct vnop_getattr_args *);
typedef int (vnop_setattr_t)        (struct vnop_setattr_args *);
typedef int (vnop_ioctl_t)          (struct vnop_ioctl_args *);
typedef int (vnop_link_t)           (struct vnop_link_args *);
typedef int (vnop_mmap_t)           (struct vnop_mmap_args *);
typedef int (vnop_mkdir_t)          (struct vnop_mkdir_args *);
typedef int (vnop_rmdir_t)          (struct vnop_rmdir_args *);
typedef int (vnop_mknod_t)          (struct vnop_mknod_args *);
typedef int (vnop_pathconf_t)       (struct vnop_pathconf_args *);
typedef int (vnop_readlink_t)       (struct vnop_readlink_args *);
typedef int (vnop_rename_t)         (struct vnop_rename_args *);
typedef int (vnop_strategy_t)       (struct vnop_strategy_args *);
typedef int (vnop_symlink_t)        (struct vnop_symlink_args *);
typedef int (vnop_whiteout_t)       (struct vnop_whiteout_args *);
typedef int (vnop_lock1_t)          (struct vnop_lock1_args *);
typedef int (vnop_fsync_t)          (struct vnop_read_args *);
typedef int (vnop_read_t)           (struct vnop_read_args *);
typedef int (vnop_write_t)          (struct vnop_write_args *);
typedef int (vnop_pagein_t)         (struct vnop_pagein_args *);
typedef int (vnop_pageout_t)        (struct vnop_pageout_args *);
typedef int (vnop_blktooff_t)       (struct vnop_blktooff_args *);
typedef int (vnop_offtoblk_t)       (struct vnop_offtoblk_args *);
typedef int (vnop_getextattr_t)     (struct vnop_getxattr_args *);
typedef int (vnop_setextattr_t)     (struct vnop_setxattr_args *);
typedef int (vnop_listextattr_t)    (struct vnop_listxattr_args *);
typedef int (vnop_deleteextattr_t)  (struct vnop_removexattr_args *);

extern VOPFUNC *ufsX_vnodeop_p;
extern VOPFUNC *ufsX_fifoop_p;
extern VOPFUNC *ufsX_specop_p;

// ufs vnops
__XNU_PRIVATE_EXTERN vnop_close_t          ufs_close;
__XNU_PRIVATE_EXTERN vnop_create_t         ufs_create;
__XNU_PRIVATE_EXTERN vnop_getattr_t        ufs_getattr;
__XNU_PRIVATE_EXTERN vnop_ioctl_t          ufs_ioctl;
__XNU_PRIVATE_EXTERN vnop_link_t           ufs_link;
__XNU_PRIVATE_EXTERN vnop_mmap_t           ufs_mmap;
__XNU_PRIVATE_EXTERN vnop_mkdir_t          ufs_mkdir;
__XNU_PRIVATE_EXTERN vnop_mknod_t          ufs_mknod;
__XNU_PRIVATE_EXTERN vnop_open_t           ufs_open;
__XNU_PRIVATE_EXTERN vnop_pathconf_t       ufs_pathconf;
__XNU_PRIVATE_EXTERN vnop_readlink_t       ufs_readlink;
__XNU_PRIVATE_EXTERN vnop_remove_t         ufs_remove;
__XNU_PRIVATE_EXTERN vnop_rename_t         ufs_rename;
__XNU_PRIVATE_EXTERN vnop_rmdir_t          ufs_rmdir;
__XNU_PRIVATE_EXTERN vnop_setattr_t        ufs_setattr;
__XNU_PRIVATE_EXTERN vnop_strategy_t       ufs_strategy;
__XNU_PRIVATE_EXTERN vnop_symlink_t        ufs_symlink;
__XNU_PRIVATE_EXTERN vnop_whiteout_t       ufs_whiteout;

__XNU_PRIVATE_EXTERN vnop_read_t           ufsfifo_read;
__XNU_PRIVATE_EXTERN vnop_write_t          ufsfifo_write;
__XNU_PRIVATE_EXTERN vnop_close_t          ufsfifo_close;

__XNU_PRIVATE_EXTERN vnop_read_t           ufsspec_read;
__XNU_PRIVATE_EXTERN vnop_write_t          ufsspec_write;
__XNU_PRIVATE_EXTERN vnop_close_t          ufsspec_close;

// ffs vnops
//__XNU_PRIVATE_EXTERN vnop_fdatasync_t      ffs_fdatasync;
__XNU_PRIVATE_EXTERN vnop_fsync_t          ffs_fsync;
//__XNU_PRIVATE_EXTERN vnop_getpages_t       ffs_getpages;
//__XNU_PRIVATE_EXTERN vnop_getpages_async_t ffs_getpages_async;
__XNU_PRIVATE_EXTERN vnop_lock1_t          ffs_lock;
__XNU_PRIVATE_EXTERN vnop_read_t           ffs_read;
__XNU_PRIVATE_EXTERN vnop_write_t          ffs_write;
__XNU_PRIVATE_EXTERN vnop_strategy_t       ffsext_strategy;
//__XNU_PRIVATE_EXTERN vnop_closeextattr_t   ffs_closeextattr;
//__XNU_PRIVATE_EXTERN vnop_openextattr_t    ffs_openextattr;
__XNU_PRIVATE_EXTERN vnop_deleteextattr_t  ffs_deleteextattr;
__XNU_PRIVATE_EXTERN vnop_getextattr_t     ffs_getextattr;
__XNU_PRIVATE_EXTERN vnop_listextattr_t    ffs_listextattr;
__XNU_PRIVATE_EXTERN vnop_setextattr_t     ffs_setextattr;
__XNU_PRIVATE_EXTERN vnop_blktooff_t       ffs_blktooff;
__XNU_PRIVATE_EXTERN vnop_offtoblk_t       ffs_offtoblk;
__XNU_PRIVATE_EXTERN vnop_pagein_t         ffs_pagein;
__XNU_PRIVATE_EXTERN vnop_pageout_t        ffs_pageout;

#include <miscfs/specfs/specdev.h>
#include <miscfs/fifofs/fifo.h>

#endif /* ufs_vnops_h */
