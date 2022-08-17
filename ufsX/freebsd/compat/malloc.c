//
//  kern_compat.c
//  ufsX
//
//  Created by John Othwolo on 7/27/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/ucred.h>

#include <freebsd/compat/malloc.h>

void*
malloc(size_t size, int type, int flags)
{
    return _MALLOC(size, type, flags);
}

void
free(void *ptr, int type)
{
    _FREE(ptr, type);
}
