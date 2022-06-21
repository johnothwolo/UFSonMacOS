//
//  mount.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef mount_h
#define mount_h

#include <sys/mount.h>
#include <freebsd/sys/util.h>

// kernel mount flags
#define    MNTK_SUSPEND     0x08000000    /* request write suspension */
#define    MNTK_SUSPEND2    0x04000000    /* block secondary writes */
#define    MNTK_SUSPENDED   0x10000000    /* write operations are suspended */

// MARK: freebsd-specific mount flags
// MARK: abstracted over noah's constant conversion macros

#define FBSD_MNT(_) \
        DECL_FBSD(_,MNT_RDONLY,         0x0000000000000001ULL) /* read only filesystem */         \
        DECL_FBSD(_,MNT_SYNCHRONOUS,    0x0000000000000002ULL) /* fs written synchronously */     \
        DECL_FBSD(_,MNT_NOEXEC,         0x0000000000000004ULL) /* can't exec from filesystem */   \
        DECL_FBSD(_,MNT_NOSUID,         0x0000000000000008ULL) /* don't honor setuid fs bits */   \
        DECL_FBSD(_,MNT_NFS4ACLS,       0x0000000000000010ULL, FBSD_SPECIFIC) /* enable NFS version 4 ACLs */    \
        DECL_FBSD(_,MNT_UNION,          0x0000000000000020ULL) /* union with underlying fs */     \
        DECL_FBSD(_,MNT_ASYNC,          0x0000000000000040ULL) /* fs written asynchronously */    \
        DECL_FBSD(_,MNT_SUIDDIR,        0x0000000000100000ULL, FBSD_SPECIFIC) /* special SUID dir handling */    \
        DECL_FBSD(_,MNT_SOFTDEP,        0x0000000000200000ULL, FBSD_SPECIFIC) /* using soft updates */           \
        DECL_FBSD(_,MNT_NOSYMFOLLOW,    0x0000000000400000ULL, FBSD_SPECIFIC) /* do not follow symlinks */       \
        DECL_FBSD(_,MNT_GJOURNAL,       0x0000000002000000ULL, FBSD_SPECIFIC) /* GEOM journal support enabled */ \
        DECL_FBSD(_,MNT_MULTILABEL,     0x0000000004000000ULL, FBSD_SPECIFIC) /* MAC support for objects */      \
        DECL_FBSD(_,MNT_ACLS,           0x0000000008000000ULL, FBSD_SPECIFIC) /* ACL support enabled */          \
        DECL_FBSD(_,MNT_NOATIME,        0x0000000010000000ULL) /* dont update file access time */ \
        DECL_FBSD(_,MNT_NOCLUSTERR,     0x0000000040000000ULL, FBSD_SPECIFIC) /* disable cluster read */         \
        DECL_FBSD(_,MNT_NOCLUSTERW,     0x0000000080000000ULL, FBSD_SPECIFIC) /* disable cluster write */        \
        DECL_FBSD(_,MNT_SUJ,            0x0000000100000000ULL, FBSD_SPECIFIC) /* using journaled soft updates */ \
        DECL_FBSD(_,MNT_AUTOMOUNTED,    0x0000000200000000ULL) /* mounted by automountd(8) */     \
        DECL_FBSD(_,MNT_UNTRUSTED,      0x0000000800000000ULL, FBSD_SPECIFIC) /* filesys metadata untrusted */   \
        /*
         * NFS export related mount flags.
         */                                                                                    \
        DECL_FBSD(_,MNT_EXRDONLY,       0x0000000000000080ULL, FBSD_SPECIFIC)  /* exported read only */           \
        DECL_FBSD(_,MNT_EXPORTED,       0x0000000000000100ULL)  /* filesystem is exported */       \
        DECL_FBSD(_,MNT_DEFEXPORTED,    0x0000000000000200ULL, FBSD_SPECIFIC)  /* exported to the world */        \
        DECL_FBSD(_,MNT_EXPORTANON,     0x0000000000000400ULL, FBSD_SPECIFIC)  /* anon uid mapping for all */     \
        DECL_FBSD(_,MNT_EXKERB,         0x0000000000000800ULL, FBSD_SPECIFIC)  /* exported with Kerberos */       \
        DECL_FBSD(_,MNT_EXPUBLIC,       0x0000000020000000ULL, FBSD_SPECIFIC)  /* public export (WebNFS) */       \
        DECL_FBSD(_,MNT_EXTLS,          0x0000004000000000ULL, FBSD_SPECIFIC)  /* require TLS */                  \
        DECL_FBSD(_,MNT_EXTLSCERT,      0x0000008000000000ULL, FBSD_SPECIFIC)  /* require TLS with client cert */ \
        DECL_FBSD(_,MNT_EXTLSCERTUSER,  0x0000010000000000ULL, FBSD_SPECIFIC)  /* require TLS with user cert */   \
        /*
         * Flags set by internal operations,
         * but visible to the user.
         * XXX some of these are not quite right.. (I've never seen the root flag set)
         */                                                                                 \
        DECL_FBSD(_,MNT_LOCAL,    0x0000000000001000ULL) /* filesystem is stored locally */ \
        DECL_FBSD(_,MNT_QUOTA,    0x0000000000002000ULL) /* quotas are enabled on fs */     \
        DECL_FBSD(_,MNT_ROOTFS,   0x0000000000004000ULL) /* identifies the root fs */       \
        DECL_FBSD(_,MNT_USER,     0x0000000000008000ULL, FBSD_SPECIFIC) /* mounted by a user */            \
        DECL_FBSD(_,MNT_IGNORE,   0x0000000000800000ULL, FBSD_SPECIFIC) /* do not show entry in df */      \
        DECL_FBSD(_,MNT_VERIFIED, 0x0000000400000000ULL, FBSD_SPECIFIC) /* filesystem is verified */

/*
 * Mask of flags that are visible to statfs().
 * XXX I think that this could now become (~(MNT_CMDFLAGS))
 * but the 'mount' program may need changing to handle this.
 */
#define FBSD_MNT_VISFLAGMASK   (FBSD_MNT_RDONLY      | FBSD_MNT_SYNCHRONOUS | FBSD_MNT_NOEXEC      | \
                                FBSD_MNT_NOSUID      | FBSD_MNT_UNION       | FBSD_MNT_SUJ         | \
                                FBSD_MNT_ASYNC       | FBSD_MNT_EXRDONLY    | FBSD_MNT_EXPORTED    | \
                                FBSD_MNT_DEFEXPORTED | FBSD_MNT_EXPORTANON  | FBSD_MNT_EXKERB      | \
                                FBSD_MNT_LOCAL       | FBSD_MNT_USER        | FBSD_MNT_QUOTA       | \
                                FBSD_MNT_ROOTFS      | FBSD_MNT_NOATIME     | FBSD_MNT_NOCLUSTERR  | \
                                FBSD_MNT_NOCLUSTERW  | FBSD_MNT_SUIDDIR     | FBSD_MNT_SOFTDEP     | \
                                FBSD_MNT_IGNORE      | FBSD_MNT_EXPUBLIC    | FBSD_MNT_NOSYMFOLLOW | \
                                FBSD_MNT_GJOURNAL    | FBSD_MNT_MULTILABEL  | FBSD_MNT_ACLS        | \
                                FBSD_MNT_NFS4ACLS    | FBSD_MNT_AUTOMOUNTED | FBSD_MNT_VERIFIED    | \
                                FBSD_MNT_UNTRUSTED)

/* Mask of flags that can be updated. */
#define    FBSD_MNT_UPDATEMASK (FBSD_MNT_NOSUID      | FBSD_MNT_NOEXEC      | FBSD_MNT_ACLS       \
                                FBSD_MNT_SYNCHRONOUS | FBSD_MNT_UNION       | FBSD_MNT_ASYNC    | \
                                FBSD_MNT_NOATIME     | FBSD_MNT_NOSYMFOLLOW | FBSD_MNT_IGNORE   | \
                                FBSD_MNT_NOCLUSTERR  | FBSD_MNT_NOCLUSTERW  | FBSD_MNT_SUIDDIR  | \
                                FBSD_MNT_AUTOMOUNTED | FBSD_MNT_USER        | FBSD_MNT_NFS4ACLS | \
                                FBSD_MNT_UNTRUSTED)


/*
 * External filesystem command modifier flags.
 * Unmount can use the MNT_FORCE flag.
 * XXX: These are not STATES and really should be somewhere else.
 * XXX: MNT_BYFSID and MNT_NONBUSY collide with MNT_ACLS and MNT_MULTILABEL,
 *      but because MNT_ACLS and MNT_MULTILABEL are only used for mount(2),
 *      and MNT_BYFSID and MNT_NONBUSY are only used for unmount(2),
 *      it's harmless.
 * MARK: MNT_SNAPSHOT is a snapshot request in freebsd...
 */
#define FBSD_MNT_CMD(_) \
        DECL_FBSD(_,MNT_UPDATE,      0x0000000000010000ULL) /* not real mount, just update */\
        DECL_FBSD(_,MNT_DELEXPORT,   0x0000000000020000ULL, FBSD_SPECIFIC) /* delete export host lists */\
        DECL_FBSD(_,MNT_RELOAD,      0x0000000000040000ULL) /* reload filesystem data */\
        DECL_FBSD(_,MNT_FORCE,       0x0000000000080000ULL) /* force unmount or readonly */\
        DECL_FBSD(_,MNT_SNAPSHOT,    0x0000000001000000ULL, FBSD_SPECIFIC) /* snapshot the filesystem */\
        DECL_FBSD(_,MNT_NONBUSY,     0x0000000004000000ULL, FBSD_SPECIFIC) /* check vnode use counts. */\
        DECL_FBSD(_,MNT_BYFSID,      0x0000000008000000ULL, FBSD_SPECIFIC) /* specify filesystem by ID. */\
        DECL_FBSD(_,MNT_NOCOVER,     0x0000001000000000ULL, FBSD_SPECIFIC) /* Do not cover a mount point */\
        DECL_FBSD(_,MNT_EMPTYDIR,    0x0000002000000000ULL, FBSD_SPECIFIC) /* Only mount on empty dir */\
/*
 * Flags for various system call interfaces.
 * 'waitfor' flags to vfs_sync() and getfsstat()
 */                                                                                                     \
        DECL_FBSD(_,MNT_WAIT,    1)   /* synchronously wait for I/O to complete */                      \
        DECL_FBSD(_,MNT_NOWAIT,  2)   /* start all I/O, but do not wait for it */                       \
        DECL_FBSD(_,MNT_LAZY,    3, FBSD_SPECIFIC)    /* push data not written by filesystem syncer */    \
        DECL_FBSD(_,MNT_SUSPEND, 4, FBSD_SPECIFIC)    /* Suspend file system after sync */
/*
        XXX: MARK: We don't need Darwin specific flags
 
        FROM_DARWN(_,MNT_DWAIT,       20)
        FROM_DARWN(_,MNT_VOLUME,      22)      * sync on a single mounted filesystem  *
*/


// declare the enums...
DECLARE_CENUM(mntflag, FBSD_MNT);
DECLARE_CENUM(mnt_cmdflag, FBSD_MNT_CMD);

#define    FBSD_MNT_CMDFLAGS    (FBSD_MNT_UPDATE  | FBSD_MNT_DELEXPORT | FBSD_MNT_RELOAD   | \
                                  FBSD_MNT_FORCE  | FBSD_MNT_SNAPSHOT  | FBSD_MNT_NONBUSY  | \
                                  FBSD_MNT_BYFSID | FBSD_MNT_NOCOVER   | FBSD_MNT_EMPTYDIR)


// undef this and rename it
#undef LK_NOWAIT
#define MBF_NOWAIT 1

inline void VFS_SUSP_CLEAN(struct mount *mp)
{
    void process_deferred_inactive(struct mount *mp);
    process_deferred_inactive(mp);
}


uint64_t   vfs_flags_fbsd(mount_t mp);
void       vfs_setflags_fbsd(mount_t mp, uint64_t flags);
void       vfs_clearflags_fbsd(mount_t mp, uint64_t flags);
void       vfs_rel(struct mount *mp);
void       vfs_ref(struct mount *mp);
int        vfs_busy_fbsd(mount_t mp, int flags);
void       vfs_unbusy_fbsd(mount_t mp);

// mntfs
struct vnode *mntfs_allocvp(struct mount *, struct vnode *);
void   mntfs_freevp(struct vnode *);

// kernel vfs routines
#define    VFS_VGET(MP, INO, FLAGS, VPP, CONTEXT) ({            \
        int _rc;                                                \
        _rc = ffs_vgetf(MP, INO, FLAGS, VPP, 0, CONTEXT);       \
        _rc;                                                    \
})

#endif /* mount_h */
