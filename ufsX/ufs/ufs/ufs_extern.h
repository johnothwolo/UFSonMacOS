/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ufs_extern.h	8.10 (Berkeley) 5/14/95
 * $FreeBSD$
 */

#ifndef _UFS_UFS_EXTERN_H_
#define	_UFS_UFS_EXTERN_H_

struct componentname;
struct direct;
struct indir;
struct inode;
struct mount;
struct thread;
struct sockaddr;
struct ucred;
struct ufid;
struct vfsconf;
struct vnode;
struct componentname;
struct ufsmount;
struct vnop_blockmap_args;
struct vnop_generic_args;
struct vnop_inactive_args;
struct vnop_reclaim_args;

#define ASSERT_VNOP_ELOCKED(vp, str)
#define ASSERT_VNOP_LOCKED(vp, str)
#define VNOP_UNLOCK(ip)    0
#define vn_lock(vp, flags) 0

enum { VINIT_VALLOC = 0x10 };

struct vnode_init_args {
    struct vnode *vi_parent;
    struct componentname *vi_cnp;
    struct inode *vi_ip;
    uint64_t vi_flags;
    bool vi_system_file;
    struct vfs_context *vi_ctx;
};

extern lck_grp_t *ffs_lock_group;

/* ufs_ihash.c */
void ufs_hash_init(void);
void ufs_hash_uninit(void);
vnode_t ufs_hash_lookup(struct ufsmount *ump, dev_t dev, ino64_t inum);
int ufs_hash_get(struct ufsmount *ump, ino64_t inum, int flags, vnode_t *vpp);
void ufs_hash_insert(struct inode *ip);
void ufs_hash_remove(struct inode *ip);


int	 ufs_bmap(struct vnop_blockmap_args *);
int	 ufs_bmaparray(struct vnode *, ufs2_daddr_t, ufs2_daddr_t *, struct buf *, int *, int *);
int	 ufs_bmap_seekdata(struct vnode *, off_t *);
int  ufs_bmap_seekhole(struct vnode *, struct vnodeop_desc *, u_long, off_t *, struct vfs_context *);
int	 ufs_fhtovp(struct mount *, int, struct ufid *, struct vnode **, vfs_context_t);
int	 ufs_checkpath(ino_t, ino_t, struct inode *, struct vfs_context*, ino_t *);
void ufs_dirbad(struct inode *, int32_t, char *);
int	 ufs_dirbadentry(struct vnode *, struct direct *, int);
int	 ufs_dirempty(struct inode *, ino_t, struct vfs_context *);
int	 ufs_extread(struct vnop_read_args *);
int	 ufs_extwrite(struct vnop_write_args *);
void ufs_makedirentry(struct inode *, struct componentname *, struct direct *);
int	 ufs_direnter(struct vnode *, struct vnode *, struct direct *,
	    struct componentname *, struct buf *, int, vfs_context_t);
int	 ufs_dirremove(struct vnode *, struct inode *, int, int);
int	 ufs_dirrewrite(struct inode *, struct inode *, ino_t, int, int);
int	 ufs_lookup_ino(struct vnode *, struct vnode **, struct componentname *, ino_t *, vfs_context_t);
int	 ufs_getlbns(struct vnode *, ufs2_daddr_t, struct indir *, int *);
int	 ufs_inactive(struct vnop_inactive_args *);
int	 ufs_init(struct vfsconf *);
void ufs_itimes(struct vnode *);
int	 ufs_lookup(struct vnop_lookup_args *);

int	 ufs_readdir(struct vnop_readdir_args *);
int	 ufs_reclaim(struct vnop_reclaim_args *);
void ffs_snapgone(struct inode *);
int  ufs_root(struct mount *, struct vnode **, struct vfs_context *);
int	 ufs_uninit(struct vfsconf *);
int  ufs_getnewvnode(mount_t mp, struct vnode_init_args *ap, vnode_t *vpp);
#include <sys/sysctl.h>
SYSCTL_DECL(_vfs_ufs);

/*
 * Soft update function prototypes.
 */
int	    softdep_setup_directory_add(struct buf *, struct inode *, off_t,
                                    ino_t, struct buf *, int);
void	softdep_change_directoryentry_offset(struct buf *, struct inode *,
                                             caddr_t, caddr_t, caddr_t, int);
void	softdep_setup_remove(struct buf *,struct inode *, struct inode *, int);
void	softdep_setup_directory_change(struct buf *, struct inode *,
                                       struct inode *, ino_t, int);
void	softdep_change_linkcnt(struct inode *);
int	    softdep_slowdown(struct vnode *);
void	softdep_setup_create(struct inode *, struct inode *);
void	softdep_setup_dotdot_link(struct inode *, struct inode *);
void	softdep_setup_link(struct inode *, struct inode *);
void	softdep_setup_mkdir(struct inode *, struct inode *);
void	softdep_setup_rmdir(struct inode *, struct inode *);
void	softdep_setup_unlink(struct inode *, struct inode *);
void	softdep_revert_create(struct inode *, struct inode *);
void	softdep_revert_link(struct inode *, struct inode *);
void	softdep_revert_mkdir(struct inode *, struct inode *);
void	softdep_revert_rmdir(struct inode *, struct inode *);


/*
 * Flags to low-level allocation routines.  The low 16-bits are reserved
 * for IO_ flags from vnode.h.
 *
 * Note: The general vfs code typically limits the sequential heuristic
 * count to 127.  See sequential_heuristic() in kern/vfs_vnops.c
 *
 * The BA_CLRBUF flag specifies that the existing content of the block
 * will not be completely overwritten by the caller, so buffers for new
 * blocks must be cleared and buffers for existing blocks must be read.
 * When BA_CLRBUF is not set the buffer will be completely overwritten
 * and there is no reason to clear them or to spend I/O fetching existing
 * data. The BA_CLRBUF flag is handled in the UFS_BALLOC() functions.
 */
#define	BA_CLRBUF	0x00010000	/* Clear invalid areas of buffer. */
#define	BA_METAONLY	0x00020000	/* Return indirect block buffer. */
#define	BA_UNMAPPED	0x00040000	/* Do not mmap resulted buffer. */
#define	BA_SEQMASK	0x7F000000	/* Bits holding seq heuristic. */
#define	BA_SEQSHIFT	24
#define	BA_SEQMAX	0x7F

#endif /* !_UFS_UFS_EXTERN_H_ */
