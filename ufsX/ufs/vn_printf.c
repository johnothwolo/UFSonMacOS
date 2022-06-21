/*-
* SPDX-License-Identifier: BSD-3-Clause
*
* Copyright (c) 1989, 1993
*    The Regents of the University of California.  All rights reserved.
* (c) UNIX System Laboratories, Inc.
* All or some portions of this file are derived from material licensed
* to the University of California by American Telephone and Telegraph
* Co. or Unix System Laboratories, Inc. and are reproduced herein with
* the permission of UNIX System Laboratories, Inc.
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
*    @(#)vfs_subr.c    8.31 (Berkeley) 5/26/95
*/

#include <sys/systm.h>
#include <sys/vnode.h>
#include <apple-internal/vnode.h>

/*
 * Print out a description of a vnode.
 */
static const char * const typename[] = {
    "VNON", "VREG", "VDIR", "VBLK", "VCHR", "VLNK", "VSOCK", "VFIFO", "VBAD", "VSTR", "VCPLX"
};

void
vn_printf(struct vnode *vp, const char *fmt, ...)
{
    va_list ap;
    char buf[256];
    struct vnode_internal *vnpriv = (struct vnode_internal *) vp;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("%p: ", (void *)vp);
    printf("type %s\n", typename[vnode_vtype(vp)]);
    printf("    usecount %d, writecount %d, refcount %d",
        vnode_usecount(vp), vnpriv->v_writecount, vnpriv->v_references);
    switch (vnode_vtype(vp)) {
    case VDIR:
        printf(" mountedhere %p\n", vnode_mountedhere(vp));
        break;
    case VCHR:
        printf(" rdev %d\n", vnode_specrdev(vp));
        break;
    case VSOCK:
            printf(" socket %p\n", vnpriv->v_un.vu_socket);
        break;
    case VFIFO:
            printf(" fifoinfo %p\n", vnpriv->v_un.vu_fifoinfo);
        break;
    default:
        printf("\n");
        break;
    }
    buf[0] = '\0';
    buf[1] = '\0';
    
    if (vnpriv->v_flag & VROOT)
        strlcat(buf, "|VROOT", sizeof(buf));
    if (vnpriv->v_flag & VTEXT)
        strlcat(buf, "|VTEXT", sizeof(buf));
    if (vnpriv->v_flag & VSYSTEM)
        strlcat(buf, "|VSYSTEM", sizeof(buf));
    if (vnpriv->v_flag & VISTTY)
        strlcat(buf, "|VISTTY", sizeof(buf));
    if (vnpriv->v_flag & VRAGE)
        strlcat(buf, "|VRAGE", sizeof(buf));
    if (vnpriv->v_flag & VBDEVVP)
        strlcat(buf, "|VBDEVVP", sizeof(buf));
    if (vnpriv->v_flag & VDEVFLUSH)
        strlcat(buf, "|VDEVFLUSH", sizeof(buf));
    if (vnpriv->v_flag & VMOUNT)
        strlcat(buf, "|VMOUNT", sizeof(buf));
    if (vnpriv->v_flag & VBWAIT)
        strlcat(buf, "|VBWAIT", sizeof(buf));
    if (vnpriv->v_flag & VSHARED_DYLD)
        strlcat(buf, "|VSHARED_DYLD", sizeof(buf));
    if (vnpriv->v_flag & VNOCACHE_DATA)
        strlcat(buf, "|VNOCACHE_DATA", sizeof(buf));
    if (vnpriv->v_flag & VSTANDARD)
        strlcat(buf, "|VSTANDARD", sizeof(buf));
    if (vnpriv->v_flag & VAGE)
        strlcat(buf, "|VAGE", sizeof(buf));
    if (vnpriv->v_flag & VRAOFF)
        strlcat(buf, "|VRAOFF", sizeof(buf));
    if (vnpriv->v_flag & VNCACHEABLE)
        strlcat(buf, "|VNCACHEABLE", sizeof(buf));
    if (vnpriv->v_flag & VISSHADOW)
        strlcat(buf, "|VISSHADOW", sizeof(buf));
    if (vnpriv->v_flag & VSWAP)
        strlcat(buf, "|VSWAP", sizeof(buf));
    
    if (vnpriv->v_flag & VNOFLUSH)
        strlcat(buf, "|VNOFLUSH", sizeof(buf));
    if (vnpriv->v_flag & VLOCKLOCAL)
        strlcat(buf, "|VLOCKLOCAL", sizeof(buf));
    if (vnpriv->v_flag & VISHARDLINK)
        strlcat(buf, "|VISHARDLINK", sizeof(buf));
    if (vnpriv->v_flag & VISUNION)
        strlcat(buf, "|VISUNION", sizeof(buf));
    if (vnpriv->v_flag & VISNAMEDSTREAM)
        strlcat(buf, "|VISNAMEDSTREAM", sizeof(buf));
    if (vnpriv->v_flag & VOPENEVT)
        strlcat(buf, "|VOPENEVT", sizeof(buf));
    if (vnpriv->v_flag & VNEEDSSNAPSHOT)
        strlcat(buf, "|VNEEDSSNAPSHOT", sizeof(buf));
    if (vnpriv->v_flag & VNOCS)
        strlcat(buf, "|VNOCS", sizeof(buf));
    if (vnpriv->v_flag & VISDIRTY)
        strlcat(buf, "|VISDIRTY", sizeof(buf));
    if (vnpriv->v_flag & VFASTDEVCANDIDATE)
        strlcat(buf, "|VFASTDEVCANDIDATE", sizeof(buf));
    if (vnpriv->v_flag & VAUTOCANDIDATE)
        strlcat(buf, "|VAUTOCANDIDATE", sizeof(buf));
    if (vnode_issystem(vp))
        strlcat(buf, "|VV_SYSTEM", sizeof(buf));
    if (vnode_issystem(vp))
        strlcat(buf, "|VV_SYSTEM", sizeof(buf));
    
    if (vnpriv->v_lflag & VL_SUSPENDED)
        strlcat(buf, "|VL_SUSPENDED", sizeof(buf));
    if (vnpriv->v_lflag & VL_DRAIN)
        strlcat(buf, "|VL_DRAIN", sizeof(buf));
    if (vnpriv->v_lflag & VL_TERMINATE)
        strlcat(buf, "|VL_TERMINATE", sizeof(buf));
    if (vnpriv->v_lflag & VL_TERMWANT)
        strlcat(buf, "|VL_TERMWANT", sizeof(buf));
    if (vnpriv->v_lflag & VL_DEAD)
        strlcat(buf, "|VL_DEAD", sizeof(buf));
    if (vnpriv->v_lflag & VL_MARKTERM)
        strlcat(buf, "|VL_MARKTERM", sizeof(buf));
    if (vnpriv->v_lflag & VL_NEEDINACTIVE)
        strlcat(buf, "|VL_NEEDINACTIVE", sizeof(buf));
    if (vnpriv->v_lflag & VL_LABEL)
        strlcat(buf, "|VL_LABEL", sizeof(buf));
    if (vnpriv->v_lflag & VL_LABELWAIT)
        strlcat(buf, "|VL_LABELWAIT", sizeof(buf));
    if (vnpriv->v_lflag & VL_LABELED)
        strlcat(buf, "|VL_LABELED", sizeof(buf));
    if (vnpriv->v_lflag & VL_LWARNED)
        strlcat(buf, "|VL_LWARNED", sizeof(buf));
    if (vnpriv->v_lflag & VL_HASSTREAMS)
        strlcat(buf, "|VL_HASSTREAMS", sizeof(buf));
    if (vnpriv->v_lflag & VNAMED_UBC)
        strlcat(buf, "|VNAMED_UBC", sizeof(buf));
    if (vnpriv->v_lflag & VNAMED_MOUNT)
        strlcat(buf, "|VNAMED_MOUNT", sizeof(buf));
    
    printf("    flags (%s)", buf + 1);
    
}
