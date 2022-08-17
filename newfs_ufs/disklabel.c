//
//  disklabel.c
//  newfs_ufs
//
//  Created by John Othwolo on 7/26/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <freebsd/disklabel.h>
#include <libkern/OSByteOrder.h>

#define le32dec OSSwapHostToLittleInt32
#define le16dec OSSwapHostToLittleInt16

void
bsd_partition_le_dec(u_char *ptr, struct partition *d)
{
    d->p_size = le32dec(ptr + 0);
    d->p_offset = le32dec(ptr + 4);
    d->p_fsize = le32dec(ptr + 8);
    d->p_fstype = ptr[12];
    d->p_frag = ptr[13];
    d->p_cpg = le16dec(ptr + 14);
}

int __attribute__((visibility("default")))
bsd_disklabel_le_dec(u_char *ptr, struct disklabel *d, int maxpart)
{
    int i;
    u_char *p, *pe;
    uint16_t sum;

    d->d_magic = le32dec(ptr + 0);
    if (d->d_magic != DISKMAGIC)
        return(EINVAL);

    d->d_magic2 = le32dec(ptr + 132);
    if (d->d_magic2 != DISKMAGIC) {
        return(EINVAL);
    }

    d->d_npartitions = le16dec(ptr + 138);
    if (d->d_npartitions > maxpart) {
        return(EINVAL);
    }

    pe = ptr + 148 + 16 * d->d_npartitions;
    sum = 0;
    for (p = ptr; p < pe; p += 2)
        sum ^= le16dec(p);
    if (sum != 0) {
        return(EINVAL);
    }

    d->d_type = le16dec(ptr + 4);
    d->d_subtype = le16dec(ptr + 6);
    bcopy(ptr + 8, d->d_typename, 16);
    bcopy(ptr + 24, d->d_packname, 16);
    d->d_secsize = le32dec(ptr + 40);
    d->d_nsectors = le32dec(ptr + 44);
    d->d_ntracks = le32dec(ptr + 48);
    d->d_ncylinders = le32dec(ptr + 52);
    d->d_secpercyl = le32dec(ptr + 56);
    d->d_secperunit = le32dec(ptr + 60);
    d->d_sparespertrack = le16dec(ptr + 64);
    d->d_sparespercyl = le16dec(ptr + 66);
    d->d_acylinders = le32dec(ptr + 68);
    d->d_rpm = le16dec(ptr + 72);
    d->d_interleave = le16dec(ptr + 74);
    d->d_trackskew = le16dec(ptr + 76);
    d->d_cylskew = le16dec(ptr + 78);
    d->d_headswitch = le32dec(ptr + 80);
    d->d_trkseek = le32dec(ptr + 84);
    d->d_flags = le32dec(ptr + 88);
    d->d_drivedata[0] = le32dec(ptr + 92);
    d->d_drivedata[1] = le32dec(ptr + 96);
    d->d_drivedata[2] = le32dec(ptr + 100);
    d->d_drivedata[3] = le32dec(ptr + 104);
    d->d_drivedata[4] = le32dec(ptr + 108);
    d->d_spare[0] = le32dec(ptr + 112);
    d->d_spare[1] = le32dec(ptr + 116);
    d->d_spare[2] = le32dec(ptr + 120);
    d->d_spare[3] = le32dec(ptr + 124);
    d->d_spare[4] = le32dec(ptr + 128);
    d->d_checksum = le16dec(ptr + 136);
    d->d_npartitions = le16dec(ptr + 138);
    d->d_bbsize = le32dec(ptr + 140);
    d->d_sbsize = le32dec(ptr + 144);
    for (i = 0; i < d->d_npartitions; i++)
        bsd_partition_le_dec(ptr + 148 + 16 * i, &d->d_partitions[i]);
    return(0);
}
