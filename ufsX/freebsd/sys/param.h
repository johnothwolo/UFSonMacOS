//
//  param.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef param_h
#define param_h

#define    nitems(x)    (sizeof((x)) / sizeof((x)[0]))
#define    rounddown(x, y)    (((x)/(y))*(y))
#define    rounddown2(x, y) ((x)&(~((y)-1)))          /* if y is power of two */
#undef     roundup
#define    roundup(x, y)    ((((x)+((y)-1))/(y))*(y))  /* to any y */
#define    roundup2(x, y)    (((x)+((y)-1))&(~((y)-1))) /* if y is powers of two */
#define    powerof2(x)    ((((x)-1)&(x))==0)

#endif /* param_h */
