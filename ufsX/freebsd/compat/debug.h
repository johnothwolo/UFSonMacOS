//
//  debug.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef debug_h
#define debug_h

#pragma mark - debugging

#ifndef _KERN_THREAD_H_
extern thread_t
current_thread(void) __attribute__((const));
#endif

void __log_debug(thread_t, const char*, int, const char*, const char*, ...) __printflike(5, 6);
#define log_debug(fmt, a...) __log_debug(current_thread(), __FILE__, __LINE__, __func__, fmt, ##a)

#define trace_return(v) ({                            \
    int __r__ = (v);                                  \
    log_debug("returning %llu", (uint64_t)(__r__));  \
    return __r__;                                     \
})

#define trace_enter() ({            \
    log_debug("ENTER");            \
})

#endif /* debug_h */
