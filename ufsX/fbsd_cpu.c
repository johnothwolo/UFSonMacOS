//
//  critical.c
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#include <libkern/OSDebug.h>

#include <stdatomic.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#include <freebsd/sys/compat.h>


#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>

static int _Thread_local td_critnest_gt;

void
critical_enter(void)
{
    td_critnest_gt++;
    __compiler_barrier();
}

void
critical_exit(void)
{
    KASSERT(td_critnest_gt != 0, ("critical_exit: td_critnest_gt == 0"));
    __compiler_barrier();
    td_critnest_gt--;
    __compiler_barrier();
}
