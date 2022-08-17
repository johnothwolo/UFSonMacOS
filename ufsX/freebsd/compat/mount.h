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

#define UNKNOWNUID ((uid_t)99)
#define UNKNOWNGID ((gid_t)99)
#define UNKNOWNPERMISSIONS (S_IRWXU | S_IROTH | S_IXOTH)        /* 705 */

// kernel mount flags
#define    MNTK_VVCOPYONWRITE   0x00000002    /* devvp is doing copy-on-write */
#define    MNTK_SOFTDEP         0x00000004    /* async disabled by softdep */
#define    MNTK_SUSPEND         0x08000000    /* request write suspension */
#define    MNTK_SUSPEND2        0x04000000    /* block secondary writes */
#define    MNTK_SUSPENDED       0x10000000    /* write operations are suspended */

// MARK: freebsd-specific mount flags

enum {
    FREEBSD_MNT_NFS4ACLS       = 0x0000000000000010ULL, /* enable NFS version 4 ACLs */
    FREEBSD_MNT_SUIDDIR        = 0x0000000000100000ULL, /* special SUID dir handling */
    FREEBSD_MNT_SOFTDEP        = 0x0000000000200000ULL, /* using soft updates */
    FREEBSD_MNT_NOSYMFOLLOW    = 0x0000000000400000ULL, /* do not follow symlinks */
    FREEBSD_MNT_GJOURNAL       = 0x0000000002000000ULL, /* GEOM journal support enabled */
    FREEBSD_MNT_ACLS           = 0x0000000008000000ULL, /* ACL support enabled */
    FREEBSD_MNT_NOCLUSTERR     = 0x0000000040000000ULL, /* disable cluster read */
    FREEBSD_MNT_NOCLUSTERW     = 0x0000000080000000ULL, /* disable cluster write */
    FREEBSD_MNT_SUJ            = 0x0000000100000000ULL, /* using journaled soft updates */
    FREEBSD_MNT_UNTRUSTED      = 0x0000000800000000ULL, /* filesys metadata untrusted */
   /*
    * NFS export related mount flags.
    */
    FREEBSD_MNT_EXRDONLY       = 0x0000000000000080ULL, /* exported read only */
    FREEBSD_MNT_DEFEXPORTED    = 0x0000000000000200ULL, /* exported to the world */
    FREEBSD_MNT_EXPORTANON     = 0x0000000000000400ULL, /* anon uid mapping for all */
    FREEBSD_MNT_EXKERB         = 0x0000000000000800ULL, /* exported with Kerberos */
    FREEBSD_MNT_EXPUBLIC       = 0x0000000020000000ULL, /* public export (WebNFS) */
    FREEBSD_MNT_EXTLS          = 0x0000004000000000ULL, /* require TLS */
    FREEBSD_MNT_EXTLSCERT      = 0x0000008000000000ULL, /* require TLS with client cert */
    FREEBSD_MNT_EXTLSCERTUSER  = 0x0000010000000000ULL, /* require TLS with user cert */
};

/*
 * Flags set by internal operations,
 * but visible to the user.
 * XXX some of these are not quite right.. (I've never seen the root flag set)
 */
enum {
    FREEBSD_MNT_USER     = 0x0000000000008000ULL, /* mounted by a user */
    FREEBSD_MNT_IGNORE   = 0x0000000000800000ULL, /* do not show entry in df */
    FREEBSD_MNT_VERIFIED = 0x0000000400000000ULL, /* filesystem is verified */
};

enum {
    FREEBSD_MNT_VISFLAGMASK = FREEBSD_MNT_NFS4ACLS | FREEBSD_MNT_SUIDDIR | FREEBSD_MNT_SOFTDEP |
                            FREEBSD_MNT_NOSYMFOLLOW | FREEBSD_MNT_GJOURNAL | FREEBSD_MNT_ACLS |
                            FREEBSD_MNT_NOCLUSTERR | FREEBSD_MNT_NOCLUSTERW | FREEBSD_MNT_SUJ |
                            /* NFS export related mount flags. */
                            FREEBSD_MNT_UNTRUSTED | FREEBSD_MNT_EXRDONLY | FREEBSD_MNT_DEFEXPORTED |
                            FREEBSD_MNT_EXPORTANON | FREEBSD_MNT_EXKERB | FREEBSD_MNT_EXPUBLIC |
                            FREEBSD_MNT_EXTLS | FREEBSD_MNT_EXTLSCERT | FREEBSD_MNT_EXTLSCERTUSER
};

/*
 * Flags for various system call interfaces.
 * 'waitfor' flags to vfs_sync() and getfsstat()
 */
#define MNT_LAZY        9     /* push data not written by filesystem syncer */
#define MNT_SUSPEND     10    /* Suspend file system after sync */

#ifdef _KERNEL

#define MNT_REF(bmp)    do {                             \
    LCK_MTX_ASSERT(bmp->mnt_lock, LCK_MTX_ASSERT_OWNED); \
    (bmp)->mnt_ref++;                                    \
} while (0)

#define MNT_REL(bmp)    do {                             \
    LCK_MTX_ASSERT(bmp->mnt_lock, LCK_MTX_ASSERT_OWNED); \
    (bmp)->mnt_ref--;                                    \
    if ((bmp)->mnt_ref == 0 && (bmp)->mnt_vfs_ops)        \
        wakeup((bmp));                                  \
} while (0)

#define MNT_MTX(bsdmount) ((bsdmount)->mnt_lock)

#define MNT_ILOCK(bsdmount)    do {                             \
    lck_mtx_lock((bsdmount)->mnt_lock);                           \
} while (0)

#define MNT_IUNLOCK(bsdmount)    do {                             \
    lck_mtx_unlock((bsdmount)->mnt_lock);                           \
} while (0)

struct bsdmount {
    lck_mtx_t   *mnt_lock;
    uint64_t    mnt_flag;
    uint64_t    mnt_kern_flag;
    int         mnt_ref;                 /* (i) Reference count for fbsd code */
    thread_t    mnt_susp_owner;          /* (i) thread that suspended writes */
    int         mnt_vfs_ops;             /* (i) pending vfs ops */
    int         mnt_secondary_writes;    /* (i) # of secondary writes */
    int         mnt_secondary_accwrites; /* (i) secondary wr. starts */
    int         mnt_writeopcount;        /* (i) write syscalls pending */
};

// undef this and rename it

static inline void VFS_SUSP_CLEAN(struct mount *mp)
{
    void process_deferred_inactive(struct mount *mp);
    process_deferred_inactive(mp);
}

bool       vfs_issoftdep(mount_t mp, int locked);
bool       vfs_issuspend(mount_t mp, int locked);
bool       vfs_issuspended(mount_t mp, int locked);
bool       vfs_issuspend2(mount_t mp, int locked);

uint64_t   vfs_freebsdflags(mount_t mp);
void       vfs_setfreebsdflags(mount_t mp, uint64_t flags);
void       vfs_clearflags_fbsd(mount_t mp, uint64_t flags);
void       vfs_rel(mount_t mp);
void       vfs_ref(mount_t mp);
int        vfs_busy_bsd(mount_t mp, int flags);
void       vfs_unbusy_bsd(mount_t mp);
void       vfs_op_enter(mount_t mp);
void       vfs_op_exit(mount_t mp);

struct vfs_vget_args {
    struct vnode* dvp;
    struct componentname *cnp;
    int flags;
};

// kernel vfs routines
int VFS_VGET(struct mount *mp, ino_t ino, struct vfs_vget_args *ap ,struct vnode **vpp, vfs_context_t context);
int VFS_SYNC(struct mount *mp, int waitfor);

char *devtoname(struct vnode *devvp);

#endif /* _KERNEL */

#endif /* mount_h */
