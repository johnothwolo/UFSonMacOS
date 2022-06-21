//
//  systm.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef systm_h
#define systm_h

#include <sys/cdefs.h>

#undef KASSERT

#define    VNASSERT(exp, vp, msg) do {                    \
    if (__improbable(exp)) {                    \
        vn_printf(vp, "VNASSERT failed: %s not true at %s:%d (%s)\n",\
           #exp, __FILE__, __LINE__, __func__);             \
        panic msg;                    \
    }                                \
} while (0)
#define    VNPASS(exp, vp)    do {                        \
    const char *_exp = #exp;                    \
    VNASSERT(exp, vp, ("condition %s not met at %s:%d (%s)",    \
        _exp, __FILE__, __LINE__, __func__));            \
} while (0)

// TODO: move this to a "debug.h"
#define KASSERT(exp, msg) do {                                            \
    if (__builtin_expect(!!(exp), 0))                                     \
        panic msg;                                                        \
} while (0)
/*
 * Helpful macros for quickly coming up with assertions with informative
 * panic messages.
 */
#define MPASS(ex)        MPASS4(ex, #ex, __FILE__, __LINE__)
#define MPASS2(ex, what)    MPASS4(ex, what, __FILE__, __LINE__)
#define MPASS3(ex, file, line)    MPASS4(ex, #ex, file, line)
#define MPASS4(ex, what, file, line)                    \
    KASSERT((ex), ("Assertion %s failed at %s:%d", what, file, line))


void critical_enter(void);
void critical_exit(void);

#endif /* systm_h */
