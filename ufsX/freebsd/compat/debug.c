//
//  log_debug.c
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

#include <ufs/ufs/dinode.h>
#include <ufs/ufs/ufs_extern.h>

static lck_spin_t *buf_lock = NULL;
static char log_buf[10240];

char *u_basename(const char *path);

void log_debug_init(){
    printf("ufsX DEBUG %p (%s, %d): %s(): entered\n", current_thread(), u_basename(__FILE__), __LINE__, __func__);
    buf_lock = lck_spin_alloc_init(ffs_lock_group, LCK_ATTR_NULL);
}

void log_debug_uninit(){
    lck_spin_destroy(buf_lock, ffs_lock_group);
    lck_spin_free(buf_lock, ffs_lock_group);
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

void __log_debug(thread_t thread, const char *file,
                  int line, const char *function,
                  const char *fmt, ...)
{
    va_list args;
    const char *filename = u_basename(file);
    
    lck_spin_lock(buf_lock);
    memset(log_buf, 0x0, sizeof(log_buf));
    va_start(args, fmt);
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);
    printf("ufsX DEBUG %p (%s, %d): %s(): %s\n",
           thread,
           filename ? filename : "",
           line,
           function ? function : "", log_buf);
    lck_spin_unlock(buf_lock);
}

#define VPRINT_PTR          "ufs-fs DEBUG %p (%s, %d): %s(): vnode=%p :%s\n"
#define VPRINT_PTR_NULL     "ufs-fs DEBUG %p (%s, %d): %s(): vnode=(null) :%s\n"

void __vn_printf(struct vnode *vp, thread_t thread, const char *file,
                 int line, const char *function, const char *fmt, ...)
{
    va_list args;
    char vnpbuf[10240] = {0};
    const char *filename = u_basename(file);
    
    va_start(args, fmt);
    vsnprintf(vnpbuf, sizeof(vnpbuf), fmt, args);
    va_end(args);
    if (vp){
        printf(VPRINT_PTR, thread,
               filename ? filename : "", line,
               function ? function : "", vp, vnpbuf);
    } else{
        printf(VPRINT_PTR_NULL, thread,
               filename ? filename : "", line,
               function ? function : "", vnpbuf);
    }
}
