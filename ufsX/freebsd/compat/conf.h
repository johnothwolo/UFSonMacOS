//
//  conf.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef conf_h
#define conf_h

typedef void    d_priv_dtor_t(void *data);
int             devfs_get_cdevpriv(dev_t, void **);
int             devfs_set_cdevpriv(dev_t, void *, d_priv_dtor_t *);
void            devfs_clear_cdevpriv(dev_t);

#endif /* conf_h */
