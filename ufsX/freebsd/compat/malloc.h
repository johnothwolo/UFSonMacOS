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
#include <sys/malloc.h>
#include <libkern/OSMalloc.h>

#ifdef     MALLOC_DECLARE
#undef     MALLOC_DECLARE
#define    MALLOC_DECLARE(type) enum { type = M_TEMP }
#endif

#ifdef     MALLOC_DEFINE
#undef     MALLOC_DEFINE
#define    MALLOC_DEFINE(type, shortdesc, longdesc)
#endif

void* malloc(size_t size, int type, int flags);
void free(void *ptr, int type);





#endif /* ufs_malloc_h */
