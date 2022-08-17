//
//  time.h
//  ufsX
//
//  Created by John Othwolo on 6/24/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef time_h
#define time_h

#include <sys/time.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

int  ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps);
long time_seconds(void);

#endif /* time_h */
