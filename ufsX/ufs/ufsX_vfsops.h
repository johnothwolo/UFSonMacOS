//
//  ufsX_vfsops.h
//  ufsX
//
//  Created by John Othwolo on 6/24/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef ufsX_vfsops_h
#define ufsX_vfsops_h

#include <sys/types.h>

struct mount;
struct vnode;
struct vfs_context;
struct vfs_attr;
struct vfsconf;

/* Ext2 supported attributes */
enum  {
    UFS_ATTR_CMN_NATIVE  = (ATTR_CMN_DEVID | ATTR_CMN_FSID | ATTR_CMN_OBJTYPE |
                             ATTR_CMN_OBJTAG | ATTR_CMN_OBJID | ATTR_CMN_OBJPERMANENTID |
                             ATTR_CMN_MODTIME | ATTR_CMN_CHGTIME | ATTR_CMN_ACCTIME |
                             ATTR_CMN_OWNERID | ATTR_CMN_GRPID | ATTR_CMN_ACCESSMASK |
                             ATTR_CMN_USERACCESS),
    UFS_ATTR_VOL_NATIVE  = (ATTR_VOL_FSTYPE | ATTR_VOL_SIGNATURE | ATTR_VOL_SIZE |
                             ATTR_VOL_SPACEFREE | ATTR_VOL_SPACEAVAIL | ATTR_VOL_IOBLOCKSIZE |
                             ATTR_VOL_OBJCOUNT |  ATTR_VOL_FILECOUNT | /* ATTR_VOL_DIRCOUNT | */
                             ATTR_VOL_MAXOBJCOUNT | ATTR_VOL_MOUNTPOINT | ATTR_VOL_NAME |
                             ATTR_VOL_MOUNTPOINT | ATTR_VOL_MOUNTFLAGS | ATTR_VOL_MOUNTEDDEVICE |
                             ATTR_VOL_CAPABILITIES | ATTR_VOL_ATTRIBUTES | ATTR_VOL_INFO),
/*  ATTR_VOL_FILECOUNT | ATTR_VOL_DIRCOUNT aren't really native, but close enough */
    UFS_ATTR_DIR_NATIVE  = (ATTR_DIR_LINKCOUNT | ATTR_DIR_ENTRYCOUNT | ATTR_DIR_MOUNTSTATUS ),
    UFS_ATTR_FILE_NATIVE = (ATTR_FILE_LINKCOUNT | ATTR_FILE_TOTALSIZE | ATTR_FILE_ALLOCSIZE |
                             ATTR_FILE_IOBLOCKSIZE | ATTR_FILE_CLUMPSIZE | ATTR_FILE_DEVTYPE ),
    
    UFS_ATTR_CMN_SUPPORTED   = UFS_ATTR_CMN_NATIVE,
    UFS_ATTR_VOL_SUPPORTED   = UFS_ATTR_VOL_NATIVE,
    UFS_ATTR_DIR_SUPPORTED   = UFS_ATTR_DIR_NATIVE,
    UFS_ATTR_FILE_SUPPORTED  = UFS_ATTR_FILE_NATIVE,
    UFS_ATTR_FORK_NATIVE     = 0,
    UFS_ATTR_FORK_SUPPORTED  = 0,
    UFS_ATTR_CMN_SETTABLE    = 0,
    UFS_ATTR_VOL_SETTABLE    = ATTR_VOL_NAME,
    UFS_ATTR_DIR_SETTABLE    = 0,
    UFS_ATTR_FILE_SETTABLE   = 0,
    UFS_ATTR_FORK_SETTABLE   = 0,
};
int ffs_mount(struct mount *, struct vnode* devvp, user_addr_t data, struct vfs_context *);
int ffs_start(struct mount *, int flags, struct vfs_context *);
int ffs_unmount(struct mount *, int mntflags, struct vfs_context *);
int ufs_root(struct mount *, struct vnode **, struct vfs_context *);
int ufs_quotactl(struct mount *, int cmds, uid_t uid, caddr_t arg, struct vfs_context *);
int ffs_getattrfs(struct mount *, struct vfs_attr *, struct vfs_context *);
int ffs_sync(struct mount *, int waitfor, struct vfs_context *);
int ffs_vget(struct mount *, ino64_t ino, struct vnode **, struct vfs_context *);
int ffs_fhtovp(struct mount *, int fhlen, unsigned char *fhp, struct vnode **, struct vfs_context *);
int ffs_vptofh(struct vnode *vp, int *fhlen, unsigned char *fhp, struct vfs_context *);
int ffs_init(struct vfsconf *);
int ffs_sysctl(int *, u_int, user_addr_t, size_t *, user_addr_t, size_t, struct vfs_context *);
int ffs_setattrfs(struct mount *, struct vfs_attr *, struct vfs_context *);
int ffs_ioctl(struct mount *, unsigned long command, caddr_t data, int flags, struct vfs_context *);
int ffs_vget_snapdir(struct mount *, struct vnode **, struct vfs_context *);

extern struct vfsops ffs_vfsops;

#endif /* ufsX_vfsops_h */
