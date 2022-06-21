/*
 *  apple.h
 *  ufsX
 *
 *  Created by John Othwolo on 6/20/22.
 *  Copyright © 2022 John Othwolo. All rights reserved.
 */
/*
 * Copyright (c) 2000-2016 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_FREE_COPYRIGHT@
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */

#ifndef apple_h
#define apple_h

#include <sys/cdefs.h>

#pragma mark -
#pragma mark Take from vnode_internal.h

/*
 * v_lflags
 */
#define VL_SUSPENDED    0x0001          /* vnode is suspended */
#define VL_DRAIN        0x0002          /* vnode is being drained */
#define VL_TERMINATE    0x0004          /* vnode is in the process of being recycled */
#define VL_TERMWANT     0x0008          /* there's a waiter  for recycle finish (vnode_getiocount)*/
#define VL_DEAD         0x0010          /* vnode is dead, cleaned of filesystem-specific info */
#define VL_MARKTERM     0x0020          /* vnode should be recycled when no longer referenced */
#define VL_NEEDINACTIVE 0x0080          /* delay VNOP_INACTIVE until iocount goes to 0 */

#define VL_LABEL        0x0100          /* vnode is marked for labeling */
#define VL_LABELWAIT    0x0200          /* vnode is marked for labeling */
#define VL_LABELED      0x0400          /* vnode is labeled */
#define VL_LWARNED      0x0800
#define VL_HASSTREAMS   0x1000          /* vnode has had at least one associated named stream vnode (may not have one any longer) */

#define VNAMED_UBC      0x2000          /* ubc named reference */
#define VNAMED_MOUNT    0x4000          /* mount point named reference */
#define VNAMED_FSHASH   0x8000          /* FS hash named reference */

/*
 * v_flags
 */
#define VROOT           0x000001        /* root of its file system */
#define VTEXT           0x000002        /* vnode is a pure text prototype */
#define VSYSTEM         0x000004        /* vnode being used by kernel */
#define VISTTY          0x000008        /* vnode represents a tty */
#define VRAGE           0x000010        /* vnode is in rapid age state */
#define VBDEVVP         0x000020        /* vnode created by bdevvp */
#define VDEVFLUSH       0x000040        /* device vnode after vflush */
#define VMOUNT          0x000080        /* mount operation in progress */
#define VBWAIT          0x000100        /* waiting for output to complete */
#define VSHARED_DYLD    0x000200        /* vnode is a dyld shared cache file */
#define VNOCACHE_DATA   0x000400        /* don't keep data cached once it's been consumed */
#define VSTANDARD       0x000800        /* vnode obtained from common pool */
#define VAGE            0x001000        /* Insert vnode at head of free list */
#define VRAOFF          0x002000        /* read ahead disabled */
#define VNCACHEABLE     0x004000        /* vnode is allowed to be put back in name cache */
#define VISSHADOW       0x008000        /* vnode is a shadow file */
#define VSWAP           0x010000        /* vnode is being used as swapfile */
#define VTHROTTLED      0x020000        /* writes or pageouts have been throttled */
/* wakeup tasks waiting when count falls below threshold */
#define VNOFLUSH        0x040000        /* don't vflush() if SKIPSYSTEM */
#define VLOCKLOCAL      0x080000        /* this vnode does adv locking in vfs */
#define VISHARDLINK     0x100000        /* hard link needs special processing on lookup and in volfs */
#define VISUNION        0x200000        /* union special processing */
#define VISNAMEDSTREAM  0x400000        /* vnode is a named stream (eg HFS resource fork) */
#define VOPENEVT        0x800000        /* if process is P_CHECKOPENEVT, then or in the O_EVTONLY flag on open */
#define VNEEDSSNAPSHOT 0x1000000
#define VNOCS          0x2000000        /* is there no code signature available */
#define VISDIRTY       0x4000000        /* vnode will need IO if reclaimed */
#define VFASTDEVCANDIDATE  0x8000000        /* vnode is a candidate to store on a fast device */
#define VAUTOCANDIDATE 0x10000000       /* vnode was automatically marked as a fast-dev candidate */

struct buflists {
    struct buf *lh_first;  /* first element */
};

struct vnode_internal {
    lck_mtx_t v_lock;                       /* vnode mutex */
    TAILQ_ENTRY(vnode) v_freelist;          /* vnode freelist */
    TAILQ_ENTRY(vnode) v_mntvnodes;         /* vnodes for mount point */
    TAILQ_HEAD(, namecache) v_ncchildren;   /* name cache entries that regard us as their parent */
    LIST_HEAD(, namecache) v_nclinks;       /* name cache entries that name this vnode */
    vnode_t  v_defer_reclaimlist;           /* in case we have to defer the reclaim to avoid recursion */
    uint32_t v_listflag;                    /* flags protected by the vnode_list_lock (see below) */
    uint32_t v_flag;                        /* vnode flags (see below) */
    uint16_t v_lflag;                       /* vnode local and named ref flags */
    uint8_t  v_iterblkflags;                /* buf iterator flags */
    uint8_t  v_references;                  /* number of times io_count has been granted */
    int32_t  v_kusecount;                   /* count of in-kernel refs */
    int32_t  v_usecount;                    /* reference count of users */
    int32_t  v_iocount;                     /* iocounters */
    void *   v_owner;                       /* act that owns the vnode */
    uint16_t v_type;                        /* vnode type */
    uint16_t v_tag;                         /* type of underlying data */
    uint32_t v_id;
    union {
        struct mount    *vu_mountedhere;/* ptr to mounted vfs (VDIR) */
        struct socket   *vu_socket;     /* unix ipc (VSOCK) */
        struct specinfo *vu_specinfo;   /* device (VCHR, VBLK) */
        struct fifoinfo *vu_fifoinfo;   /* fifo (VFIFO) */
        struct ubc_info *vu_ubcinfo;    /* valid for (VREG) */
    } v_un;
    struct  buflists v_cleanblkhd;          /* clean blocklist head */
    struct  buflists v_dirtyblkhd;          /* dirty blocklist head */
    struct klist v_knotes;                  /* knotes attached to this vnode */
    /*
     * the following 4 fields are protected
     * by the name_cache_lock held in
     * excluive mode
     */
    kauth_cred_t    v_cred;                 /* last authorized credential */
    kauth_action_t  v_authorized_actions;   /* current authorized actions for v_cred */
    int             v_cred_timestamp;       /* determine if entry is stale for MNTK_AUTH_OPAQUE */
    int             v_nc_generation;        /* changes when nodes are removed from the name cache */
    /*
     * back to the vnode lock for protection
     */
    int32_t         v_numoutput;                    /* num of writes in progress */
    int32_t         v_writecount;                   /* reference count of writers */
    const char *v_name;                     /* name component of the vnode */
    vnode_t v_parent;                       /* pointer to parent vnode */
    struct lockf    *v_lockf;               /* advisory lock list head */
    int(**v_op)(void *);                    /* vnode operations vector */
    mount_t v_mount;                        /* ptr to vfs we are in */
    void *  v_data;                         /* private data for fs */
#if CONFIG_MACF
    struct label *v_label;                  /* MAC security label */
#endif
#if CONFIG_TRIGGERS
    vnode_resolve_t v_resolve;              /* trigger vnode resolve info (VDIR only) */
#endif /* CONFIG_TRIGGERS */
};

/*
 * This macro is very helpful in defining those offsets in the vdesc struct.
 * This is stolen from X11R4, which is under a permissive NON-APSL license...
 */
#define VDESC_NO_OFFSET                  -1
#define VOPARG_OFFSET(p_type, field)    ((int) (((char *) (&(((p_type)NULL)->field))) - ((char *) NULL)))
#define VOPARG_OFFSETOF(s_type, field)  VOPARG_OFFSET(s_type*,field)
#define VOPARG_OFFSETTO(S_TYPE, S_OFFSET, STRUCT_P) ((S_TYPE)(((char*)(STRUCT_P))+(S_OFFSET)))


#pragma mark -

#endif /* apple_h */
