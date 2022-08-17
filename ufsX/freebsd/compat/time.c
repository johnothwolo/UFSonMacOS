//
//  time.c
//  ufsX
//
//  Created by John Othwolo on 7/27/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//


#include <sys/time.h>
#include <kern/clock.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <freebsd/compat/compat.h>
static int clockinfo_set = 0;
static struct clockinfo clockinfo = { -1 };
static size_t clockinfo_size;

static struct clockinfo*
get_clockinfo(){
    if (clockinfo_set) return &clockinfo;
    int ret = sysctlbyname("kern.clockrate", &clockinfo, &clockinfo_size, NULL, 0);
    if (!ret) {
        clockinfo.hz = 100;
        clockinfo.tick = 10000;
        clockinfo.tickadj = 0;
        clockinfo.profhz = 100;
        clockinfo.stathz = 100;
    }
    clockinfo_set = 1;
    return &clockinfo;
}

/*
 * ppsratecheck(): packets (or events) per second limitation.
 *
 * Return 0 if the limit is to be enforced (e.g. the caller
 * should drop a packet because of the rate limitation).
 *
 * maxpps of 0 always causes zero to be returned.  maxpps of -1
 * always causes 1 to be returned; this effectively defeats rate
 * limiting.
 *
 * Note that we maintain the struct timeval for compatibility
 * with other bsd systems.  We reuse the storage and just monitor
 * clock ticks for minimal overhead.
 */

int
ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
    int now;
    /*
     * Reset the last time and counter if this is the first call
     * or more than a second has passed since the last update of
     * lasttime.
     */
    now = (int)mach_absolute_time() / NSEC_PER_SEC;
    if (lasttime->tv_sec == 0 || (u_int)(now - lasttime->tv_sec) >= get_clockinfo()->hz) {
        lasttime->tv_sec = now;
        *curpps = 1;
        return (maxpps != 0);
    } else {
        (*curpps)++;        /* NB: ignore potential overflow */
        return (maxpps < 0 || *curpps <= maxpps);
    }
}

int fbsd_hz(){
    return get_clockinfo()->hz;
}

int fbsd_tick(){
    clock_nsec_t mtime; // m for micro
    // microseconds is set last, so the value of mtime will change twice.
    clock_get_system_microtime((clock_t*)&mtime, &mtime);
    return mtime/get_clockinfo()->tick;
    // returns uptime in microseconds ÷ number of mircroseconds per tick
    // basically number of tick
}

long time_seconds(){
    clock_t stime, mtime;
    clock_get_system_microtime(&stime, (clock_nsec_t*)&mtime);
    return stime;
}

// TODO:...
int
fbsd_msleep(){
    return 0;
}

