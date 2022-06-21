//
//  ufs_malloc.h
//  ufsX
//
//  Created by John Othwolo on 6/19/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef ufs_malloc_h
#define ufs_malloc_h

#include <sys/sysctl.h>

#define M_VERSION 2020110501

/*
 * Public data structure describing a malloc type.
 */
struct malloc_type {
    struct malloc_type  *ks_next;    /* Next in global chain. */
    u_long               ks_version;    /* Detect programmer error. */
    const char          *ks_shortdesc;    /* Printable type name. */
};


#define    UFS_MALLOC_DEFINE(type, shortdesc, longdesc)         \
struct malloc_type type[1] = {                                  \
    {                                                           \
        .ks_next = NULL,                                        \
        .ks_version = M_VERSION,                                \
        .ks_shortdesc = shortdesc,                              \
    }                                                           \
};



#endif /* ufs_malloc_h */
