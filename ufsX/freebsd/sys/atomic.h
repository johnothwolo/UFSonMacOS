//
//  atomic.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef atomic_h
#define atomic_h

#include <libkern/OSAtomic.h>

// atomic_add_int
#define atomic_add_int(a, v)  OSAddAtomic((SInt32)(v), (a))
#define atomic_add_long(a, v) OSAddAtomic64((SInt64)(v), (a))

#endif /* atomic_h */
