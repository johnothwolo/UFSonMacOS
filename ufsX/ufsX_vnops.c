//
//  vnops.c
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/mount.h>
#include <sys/priv.h>
#include <sys/vnode.h>
#include <sys/unistd.h>
#include <sys/dirent.h>
#include <sys/lockf.h>
#include <sys/fsctl.h>
#include <sys/conf.h>
#include <sys/ubc.h>
#include <vfs/vfs_support.h>

#include <freebsd/sys/compat.h>

#include <ufs/ufsX_vnops.h>
#include <ufs/ufs/acl.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>
#include <ufs/ffs/ffs_extern.h>

// VNOP vectors

/* Global vfs data structures for ufs. */
VOPFUNC *ufsX_vnodeop_p;
struct vnodeopv_entry_desc ufsX_vnodeops[] = {
    { &vnop_default_desc,           (VOPFUNC)vn_default_error   },
    { &vnop_fsync_desc,             (VOPFUNC)err_fsync          },
    { &vnop_read_desc,              (VOPFUNC)err_read           },
    { &vnop_write_desc,             (VOPFUNC)err_write          },
    { &vnop_blockmap_desc,          (VOPFUNC)ufs_bmap           },
    { &vnop_close_desc,             (VOPFUNC)ufs_close          },
    { &vnop_create_desc,            (VOPFUNC)ufs_create         },
    { &vnop_getattr_desc,           (VOPFUNC)ufs_getattr        },
    { &vnop_inactive_desc,          (VOPFUNC)ufs_inactive       },
    { &vnop_ioctl_desc,             (VOPFUNC)ufs_ioctl          },
    { &vnop_link_desc,              (VOPFUNC)ufs_link           },
    { &vnop_lookup_desc,            (VOPFUNC)ufs_lookup         },
    { &vnop_mmap_desc,              (VOPFUNC)ufs_mmap           },
    { &vnop_mkdir_desc,             (VOPFUNC)ufs_mkdir          },
    { &vnop_mknod_desc,             (VOPFUNC)ufs_mknod          },
    { &vnop_open_desc,              (VOPFUNC)ufs_open           },
    { &vnop_pathconf_desc,          (VOPFUNC)ufs_pathconf       },
    { &vnop_readdir_desc,           (VOPFUNC)ufs_readdir        },
    { &vnop_readlink_desc,          (VOPFUNC)ufs_readlink       },
    { &vnop_reclaim_desc,           (VOPFUNC)ufs_reclaim        },
    { &vnop_remove_desc,            (VOPFUNC)ufs_remove         },
    { &vnop_rename_desc,            (VOPFUNC)ufs_rename         },
    { &vnop_rmdir_desc,             (VOPFUNC)ufs_rmdir          },
    { &vnop_setattr_desc,           (VOPFUNC)ufs_setattr        },
    { &vnop_strategy_desc,          (VOPFUNC)ufs_strategy       },
    { &vnop_symlink_desc,           (VOPFUNC)ufs_symlink        },
    { &vnop_whiteout_desc,          (VOPFUNC)ufs_whiteout       },
#ifdef UFS_EXTATTR
    { &vnop_getextattr_desc,        (VOPFUNC)ufs_getextattr     },
    { &vnop_deleteextattr_desc,     (VOPFUNC)ufs_deleteextattr  },
    { &vnop_setextattr_desc,        (VOPFUNC)ufs_setextattr     },
#endif
#ifdef UFS_ACL
    { &vnop_getacl_desc,            (VOPFUNC)ufs_getacl         },
    { &vnop_setacl_desc,            (VOPFUNC)ufs_setacl         },
    { &vnop_aclcheck_desc,          (VOPFUNC)ufs_aclcheck       },
#endif
    { NULL, NULL },
};
struct vnodeopv_desc ufsX_vnodeop_opv_desc = { &ufsX_vnodeop_p, ufsX_vnodeops };

VOPFUNC *ufsX_specop_p;
struct vnodeopv_entry_desc ufsX_specops[] = {
    { &vnop_default_desc,           (VOPFUNC)vn_default_error   },
    { &vnop_lookup_desc,            (VOPFUNC)spec_lookup        },
    { &vnop_create_desc,            (VOPFUNC)spec_create        },
    { &vnop_mknod_desc,             (VOPFUNC)spec_mknod         },
    { &vnop_open_desc,              (VOPFUNC)spec_open          },
    { &vnop_close_desc,             (VOPFUNC)ufsspec_close      },
    { &vnop_getattr_desc,           (VOPFUNC)ufs_getattr        },
    { &vnop_setattr_desc,           (VOPFUNC)ufs_setattr        },
    { &vnop_read_desc,              (VOPFUNC)ufsspec_read       },
    { &vnop_write_desc,             (VOPFUNC)ufsspec_write      },
    { &vnop_ioctl_desc,             (VOPFUNC)spec_ioctl         },
    { &vnop_select_desc,            (VOPFUNC)spec_select        },
    { &vnop_revoke_desc,            (VOPFUNC)spec_revoke        },
    { &vnop_mmap_desc,              (VOPFUNC)spec_mmap          },
    { &vnop_fsync_desc,             (VOPFUNC)ffs_fsync          },
    { &vnop_remove_desc,            (VOPFUNC)spec_remove        },
    { &vnop_link_desc,              (VOPFUNC)spec_link          },
    { &vnop_rename_desc,            (VOPFUNC)spec_rename        },
    { &vnop_mkdir_desc,             (VOPFUNC)spec_mkdir         },
    { &vnop_rmdir_desc,             (VOPFUNC)spec_rmdir         },
    { &vnop_symlink_desc,           (VOPFUNC)spec_symlink       },
    { &vnop_readdir_desc,           (VOPFUNC)spec_readdir       },
    { &vnop_readlink_desc,          (VOPFUNC)spec_readlink      },
    { &vnop_inactive_desc,          (VOPFUNC)ufs_inactive       },
    { &vnop_reclaim_desc,           (VOPFUNC)ufs_reclaim        },
    { &vnop_strategy_desc,          (VOPFUNC)spec_strategy      },
    { &vnop_pathconf_desc,          (VOPFUNC)spec_pathconf      },
    { &vnop_advlock_desc,           (VOPFUNC)err_advlock        },
    { &vnop_bwrite_desc,            (VOPFUNC)vn_bwrite          },
    { &vnop_pagein_desc,            (VOPFUNC)ffs_pagein         },
    { &vnop_pageout_desc,           (VOPFUNC)ffs_pageout        },
    { &vnop_copyfile_desc,          (VOPFUNC)err_copyfile       },
    { &vnop_blktooff_desc,          (VOPFUNC)ffs_blktooff       },
    { &vnop_offtoblk_desc,          (VOPFUNC)ffs_offtoblk       },
    { &vnop_blockmap_desc,          (VOPFUNC)err_blockmap       },
    { NULL, NULL },
};
struct vnodeopv_desc ufsX_specop_opv_desc = { &ufsX_specop_p, ufsX_specops };

VOPFUNC *ufsX_fifoop_p;
struct vnodeopv_entry_desc ufsX_fifoops[] = {
    { &vnop_default_desc,           (VOPFUNC)vn_default_error   },
    { &vnop_fsync_desc,             (VOPFUNC)err_fsync          },
    { &vnop_close_desc,             (VOPFUNC)ufsfifo_close      },
    { &vnop_getattr_desc,           (VOPFUNC)ufs_getattr        },
    { &vnop_inactive_desc,          (VOPFUNC)ufs_inactive       },
    { &vnop_pathconf_desc,          (VOPFUNC)ufs_pathconf       },
    { &vnop_read_desc,              (VOPFUNC)err_read           },
    { &vnop_reclaim_desc,           (VOPFUNC)ufs_reclaim        },
    { &vnop_setattr_desc,           (VOPFUNC)ufs_setattr        },
    { &vnop_write_desc,             (VOPFUNC)err_write          },
#ifdef UFS_EXTATTR
    { &vnop_getextattr_desc,        (VOPFUNC)ufs_getextattr     },
    { &vnop_deleteextattr_desc,     (VOPFUNC)ufs_deleteextattr  },
    { &vnop_setextattr_desc,        (VOPFUNC)ufs_setextattr     },
#endif
#ifdef UFS_ACL
    { &vnop_getacl_desc,            (VOPFUNC)ufs_getacl         },
    { &vnop_setacl_desc,            (VOPFUNC)ufs_setacl         },
    { &vnop_aclcheck_desc,          (VOPFUNC)ufs_aclcheck       },
#endif
    { NULL, NULL },
};
struct vnodeopv_desc ufsX_fifoop_opv_desc = { &ufsX_fifoop_p, ufsX_fifoops };
