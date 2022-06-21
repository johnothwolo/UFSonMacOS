//
//  dirent.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef dirent_h
#define dirent_h

#include <sys/dirent.h>

/*
 * The _GENERIC_DIRSIZ macro gives the minimum record length which will hold
 * the directory entry.  This returns the amount of space in struct dirent
 * without the d_name field, plus enough space for the name with a terminating
 * null byte (dp->d_namlen+1), rounded up to a 8 byte boundary.
 *
 * XXX although this macro is in the implementation namespace, it requires
 * a manifest constant that is not.
 */
#define    _GENERIC_DIRLEN(namlen)                    \
            ((__offsetof(struct dirent, d_name) + (namlen) + 1 + 7) & ~7)
#define    _GENERIC_DIRSIZ(dp)    _GENERIC_DIRLEN((dp)->d_namlen)
#define    GENERIC_DIRSIZ(dp)     _GENERIC_DIRSIZ(dp)
/*
 * Ensure that padding bytes are zeroed and that the name is NUL-terminated.
 */
static inline void
dirent_terminate(struct dirent *dp)
{
    memset(dp->d_name + dp->d_namlen, 0,
        dp->d_reclen - (__offsetof(struct dirent, d_name) + dp->d_namlen));
}

#define _PC_ACL_PATH_MAX    _PC_XATTR_SIZE_BITS


#endif /* dirent_h */
