//
//  buf.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef buf_h
#define buf_h

// private 'x' flags for buf_t
// freebsd says they're fs-private data, sorta like buf_fsprivate()
#define    BX_VNDIRTY    0x00000001    /* On vnode dirty list */
#define    BX_VNCLEAN    0x00000002    /* On vnode clean list */
#define    BX_CVTENXIO    0x00000004    /* Convert errors to ENXIO */
#define    BX_BKGRDWRITE    0x00000010    /* Do writes in background */
#define    BX_BKGRDMARKER    0x00000020    /* Mark buffer for splay tree */
#define    BX_ALTDATA    0x00000040    /* Holds extended data */
#define    BX_FSPRIV    0x00FF0000    /* Filesystem-specific flags mask */

int  buf_xflags(buf_t bp);
void buf_clrxflags(buf_t bp);
void buf_setxflags(buf_t bp, int flags);

// get a buf incore/in cache
buf_t incore(struct vnode *vp, daddr64_t bn);
#define buf_qrelse buf_brelse

#endif /* buf_h */
