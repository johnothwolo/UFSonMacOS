//
//  main.c
//  debugfs
//
//  Created by John Othwolo on 7/31/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//
// This is just for debugging.

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/disk.h>
#include <freebsd/disklabel.h>
#include <sys/file.h>
#include <sys/mount.h>

#include <ufs/ufs/dir.h>
#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <paths.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <libufs.h>

int main(int argc, const char * argv[]) {
    // insert code here...
    FILE *fp;
    int devfd, ret;
    char sb_buffer[SBLOCKSIZE] = {0};
    struct fs *superfs;
    size_t size, count, og_size;
    uint64_t offset;
    
    if (argc < 2)
        abort();
    
    fp = fopen(argv[1], "r");
    devfd = fileno(fp);
//    ret = (int)pread(devfd, &sb_buffer[0], SBLOCKSIZE, SBLOCK_UFS2);
    
//    if (ret != sizeof(sb_buffer)){
//        printf("pread Error!: %s", strerror(errno));
//        goto out;
//    }
//    superfs = &sb_buffer[0];
//
//    size = lseek(devfd, 0, SEEK_SET);
//    size = lseek(devfd, 0, SEEK_END);
//    printf("disk-size=%zu", size);
    
    if (ioctl(devfd, DKIOCGETBLOCKSIZE, &og_size) < 0){
        perror("couldn't get sector size");
        abort();
    }
    
    if (ioctl(devfd, DKIOCGETBLOCKCOUNT, &count) < 0){
        perror("couldn't get sector count");
        abort();
    }
    
    size = og_size * count;
    ret = fscanf(fp, "%llu", &offset);
    
    
out:
    close(devfd);
    return 0;
}

// todo find fs_fsbtodb and use that to convert ff_use_bread location.
