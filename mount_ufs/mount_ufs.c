/*
* Copyright 2003-2004 Brian Bergstrand.
*
* Redistribution and use in source and binary forms, with or without modification,
* are permitted provided that the following conditions are met:
*
* 1.    Redistributions of source code must retain the above copyright notice, this list of
*     conditions and the following disclaimer.
* 2.    Redistributions in binary form must reproduce the above copyright notice, this list of
*     conditions and the following disclaimer in the documentation and/or other materials provided
*     with the distribution.
* 3.    The name of the author may not be used to endorse or promote products derived from this
*     software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
* AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
* THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/
/*-
 * Copyright (c) 1993, 1994
 *    The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
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
 */
/*
 *  main.c
 *  mount_ufs
 *
 *  Created by John Othwolo on 6/24/22.
 *  Copyright © 2022 John Othwolo. All rights reserved.
 */

static const char whatid[] __attribute__ ((unused)) =
"@(#)Revision: $Revision: 1.17 $ Built: " __DATE__" "__TIME__;

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/stat.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <libgen.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <mntopts.h>
#include <freebsd/compat/mount.h>
#include <ufs/ufs/ufsmount.h>

#ifndef VNOVAL
#define VNOVAL  (-1)
#endif

struct mntopt mopts[] = {
    MOPT_ASYNC,
    MOPT_NOATIME,
    MOPT_NOEXEC,
    MOPT_IGNORE_OWNERSHIP,
    MOPT_GROUPQUOTA,
    
    MOPT_RDONLY,
    MOPT_BROWSE,
    MOPT_FSTAB_COMPAT,
    MOPT_STDOPTS,
    MOPT_FORCE,
    MOPT_SYNC,
    MOPT_UPDATE,
    MOPT_RELOAD,
    { NULL }
};

#define MOPT_SUIDDIR            { "suiddir",                0, 0, 0 } // opposite of nosuid
#define MOPT_SNAPSHOT           { "snapshot",               0, MNT_SNAPSHOT,   0 }
#define MOPT_MULTILABEL         { "multilabel",             0, MNT_MULTILABEL, 0 }
#define MOPT_ACLS               { "acls",                   0, FREEBSD_MNT_ACLS,       0 }
#define MOPT_NFS4ACLS           { "nfsv4acls",              0, FREEBSD_MNT_NFS4ACLS,   0 }
#define MOPT_SOFTDEP            { "soft-updates",           0, FREEBSD_MNT_SOFTDEP,    0 }
#define MOPT_SUJ                { "journaled soft-updates", 0, FREEBSD_MNT_SUJ,        0 }

struct mntopt ufs_mopts[] = {
    MOPT_SUIDDIR,
    MOPT_SNAPSHOT,
    MOPT_MULTILABEL,
    MOPT_ACLS,
    MOPT_NFS4ACLS,
    MOPT_SOFTDEP,
    MOPT_SUJ,
    NULL
};

static void     usage(void) __dead;
static mode_t   a_mask(char *s);
static int      a_uid(char *s);
static int      a_gid(char *s);

static char *progname;

int
main(int argc, char *argv[]) {
    struct vfsconf vconf;
    SCDynamicStoreRef dynStoreRef;
    struct ufs_args args = { 0, VNOVAL, VNOVAL, 0, 0 };
    char mntpath[MAXPATHLEN] = {0};
    int ch;
    uint64_t mntflags;
    MNT_DEFWRITE;
    progname = basename(argv[0]);
    if (!progname)
       progname = argv[0];
    
    mntflags = 0;
    while ((ch = getopt(argc, argv, "o:m:u:g")) != -1){
        switch (ch) {
            case 'o':
                getmntopts(optarg, mopts, &mntflags, 0);
                getmntopts(optarg, mopts, &args.ufs_mntflags, 0); // get freebsd specific mount flags
                break;
            case 'm':
                args.ufs_mask = a_mask(optarg);
                break;
            case 'u':
                args.ufs_uid = a_uid(optarg);
                break;
            case 'g':
                args.ufs_gid = a_gid(optarg);
                break;
            case '?': // i don' think this is ever executed
            default:
                usage();
        }
    }
    argc -= optind;
    argv += optind;
    
    if (argc != 2)
        usage();
    
//    argv[0];    /* the name of the device file */
//    argv[1];    /* the mount point */
    
    // check destination mount path
    if (strlen(argv[1]) >= MAXPATHLEN)
        exit (EINVAL);
    
    /*
     * Resolve the mountpoint with realpath(3) and remove unnecessary
     * slashes from the devicename if there are any.
     */
    rmslashes(argv[0], args.ufs_fspec);
    checkpath(argv[1], mntpath);
    args.fspec = &args.ufs_fspec[0];
    
    /*
     * Setup uid/gid if we're a new mount.
     * Inherit either from args else from console. if there's no console user, set UNKNOWN*ID.
     * In short:
     *  1. UID/GID args, else
     *  2. Console UID/GID, else
     *  3. UNKNOWN*ID
     */
    if ((mntflags & MNT_UPDATE) == 0) {
        dynStoreRef = SCDynamicStoreCreate (kCFAllocatorDefault, CFSTR("ext2fs"), NULL, NULL);
        CFStringRef consoleUser;
        int *gidp, *uidp, ignore;
        
        
        // if the args specify uid or gid, ignore the console uid or gid
        uidp = (args.ufs_uid != VNOVAL) ? &ignore : &args.ufs_uid;
        gidp = (args.ufs_gid != VNOVAL) ? &ignore : &args.ufs_gid;
        
        /* If dynStoreRef happens to be NULL for some reason, a temp session will be created */
        consoleUser = SCDynamicStoreCopyConsoleUser(dynStoreRef, (uint*)uidp, (uint*)gidp);
        if (consoleUser) {
            /* Somebody is on the console */
            CFRelease(consoleUser);
        } else {
            /* No user logged in */
            *uidp = UNKNOWNUID;
            *gidp = UNKNOWNGID;
        }
        
        if (dynStoreRef)
            CFRelease(dynStoreRef);
        
        if (mntflags & MNT_IGNORE_OWNERSHIP){
            if (args.ufs_mask == (mode_t)VNOVAL) args.ufs_mask = ACCESSPERMS;  /* 0777 */
        } else {
            /* just inherit mask from mount path like hfs does */
            struct stat st;
            stat(mntpath, &st);
            
            if (args.ufs_mask == (mode_t)VNOVAL)
                args.ufs_mask = st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
        }
    }

    if (getvfsbyname(UFS_NAME, &vconf))         /* Is it already loaded? */
        errx(EX_OSERR, UFS_NAME " filesystem is not available");
    
    if (mount(UFS_NAME, mntpath, (int)mntflags, &args) < 0)
        err(EX_OSERR, "%s on %s", args.ufs_fspec, mntpath);
    
    exit(0);
}

static void usage()
{
    fprintf(stderr, "UFS mount v1.1\n");
    fprintf(stderr, "usage: %s [-o options] device mount-point\n", progname);
    fprintf(stderr, "Supported options:                      \n");
    fprintf(stderr, "   suiddir                  special SUID dir handling                \n");
    fprintf(stderr, "   snapshot                 mount snapshot                           \n");
    fprintf(stderr, "   acls                     mount with acls enabled                  \n");
    fprintf(stderr, "   nfsv4acls                mount with nfs4 acls (unsupported)       \n");
    fprintf(stderr, "   soft-updates             mount with soft updates enabled          \n");
    fprintf(stderr, "   journaled soft-updates   mount with journaled soft updates enabled\n");

    exit(EX_USAGE);
}

static mode_t a_mask(char *s)
{
    int done, rv;
    char *ep = NULL;
    
    done = 0;
    rv = -1;
    if (*s >= '0' && *s <= '7') {
        long mask;
        
        done = 1;
        mask = strtol(optarg, &ep, 8);
        if (mask >= 0 && mask <= INT_MAX)
            rv = (int)mask;
    }
    if (!done || rv < 0 || *ep)
        errx(1, "invalid file mode: %s", s);
    return (rv);
}

static int a_uid(char *str)
{
    char *s = str, *user;
    struct passwd *pwd;
    
    if (s != NULL) {
        if(*s == '-')
            s++;
        for (user = s; isdigit(*s) && s != NULL; s++);
        if(NULL != (pwd = getpwnam(user))){
            return pwd->pw_uid;
        } else {
            errx(1, "unknown group id: %s", str);
        }
    }
    return -1;
}

static int a_gid(char *str)
{
    char *s = str, *group;
    struct group *gp;
    
    if (s != NULL) {
        if(*s == '-')
            s++;
        for (group = s; isdigit(*s) && s != NULL; s++);
        if(NULL != (gp = getgrnam(group))){
            return gp->gr_gid;
        } else {
            errx(1, "unknown group id: %s", str);
        }
    }
    return -1;
}
