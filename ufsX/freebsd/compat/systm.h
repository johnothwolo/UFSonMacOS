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
#include <sys/systm.h>

struct thread;

#define ERECYCLE    (-5)                /* restart lookup under heavy vnode pressure/recycling */

#define ASSERT(exp, msg) do {                                            \
                             if (__improbable(!(exp)))                   \
                                 panic msg;                              \
                         } while (0)

#define MPASS(ex)                       MPASS4(ex, #ex, __FILE__, __LINE__)
#define MPASS2(ex, what)                MPASS4(ex, what, __FILE__, __LINE__)
#define MPASS3(ex, file, line)          MPASS4(ex, #ex, file, line)
#define MPASS4(ex, what, file, line)    ASSERT((ex), ("Assertion %s failed at %s:%d", what, file, line))

#define MALLOC_DECLARE(type) enum { type = M_TEMP }
#undef  MALLOC_DEFINE
#define MALLOC_DEFINE(type, shortdesc, longdesc)



#define VNASSERT(exp, vp, msg)  do {                                                                 \
                                    if (__improbable(!(exp))) {                                      \
                                        vn_printf(vp, "VNASSERT failed: %s not true at %s:%d (%s)\n",\
                                        #exp, __FILE__, __LINE__, __func__);                         \
                                        panic msg;                                                   \
                                    }                                                                \
                                } while (0)
#define VNPASS(exp, vp)     do {                                                            \
                                const char *_exp = #exp;                                    \
                                VNASSERT(exp, vp, ("condition %s not met at %s:%d (%s)",    \
                                    _exp, __FILE__, __LINE__, __func__));                   \
                            } while (0)


void  qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *));
void* memcchr(const void *begin, int c, size_t n);
int   heapsort(void *vbase, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

// misc
int fbsd_hz(void);
int fbsd_tick(void);

static inline void
td_softdep_cleanup(struct vfs_context *context)
{
    extern void (*softdep_ast_cleanup)(struct vfs_context *);
    if (softdep_ast_cleanup != NULL)
        softdep_ast_cleanup(context);
}

void
hashdestroy(void *, int type, __unused size_t);

// ctype functions

static inline int
isupper(int c)
{
    return (c >= 'A' && c <= 'Z');
}

static inline int
islower(int c)
{
    return (c >= 'a' && c <= 'z');
}

static inline int
isalpha(int c)
{
    return (isupper(c) || islower(c));
}

static inline int
isdigit(int c)
{
    return (c >= '0' && c <= '9');
}

static inline int
isalnum(int c)
{
    return (isdigit(c) || isalpha(c));
}



#endif /* systm_h */
