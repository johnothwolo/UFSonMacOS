//
//  ufs_debug.c
//  ufsX
//
//  Created by John Othwolo on 6/18/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <libkern/OSDebug.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <kern/debug.h>
#include <sys/vnode.h>

static lck_spin_t buf_lock = {0};
static char log_buf[10240];

void ufs_debug_init(){
     lck_spin_init(&buf_lock, LCK_GRP_NULL, LCK_ATTR_NULL);
}

void ufs_debug_uninit(){
    lck_spin_destroy(&buf_lock, LCK_GRP_NULL);
}

char*
u_basename(const char *path)
{
    char *p = (char*)path;
    if (p) {
        for (size_t len = strlen(path); len > 0; len--) {
            if (p[len - 1] == '/') {
                p += len;
                break;
            }
        }
    }
    return p;
}

void __ufs_debug(thread_t thread, const char *file,
                  int line, const char *function,
                  const char *fmt, ...)
{
    va_list args;
    const char *filename = u_basename(file);
    
    lck_spin_lock(&buf_lock);
    memset(log_buf, 0x0, sizeof(log_buf));
    va_start(args, fmt);
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);
    printf("ufs-fs DEBUG %p (%s, %d): %s(): %s\n", thread, filename ? filename : "",
            line, function ? function : "", log_buf);
    lck_spin_unlock(&buf_lock);
}
