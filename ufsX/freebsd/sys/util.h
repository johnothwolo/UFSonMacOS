//
//  util.h
//  ufsX
//
//  Created by John Othwolo on 6/21/22.
//  Copyright © noah on Mac.
//

/*
 *  Constant conversion macros.
 */

#ifndef util_h
#define util_h


#define DECL_FBSD_fbsd_to_darwin(fbsd, flag, darwin, ...) if (flags & (fbsd)) ret |= (darwin);
#define DECL_ALIAS_fbsd_to_darwin(fbsd, darwin)
#define FROM_DARWN_fbsd_to_darwin(fbsd, darwin)

#pragma mark THIS MAPS BITWISE FLAGS, NOT 'SWITCH' VALUES !!!!!
#define DECL_FBSD_darwin_to_fbsd(fbsd, flag, darwin, ...) if (flags & (darwin)) ret |= (fbsd);
#define DECL_ALIAS_darwin_to_fbsd(fbsd, darwin)
#define FROM_DARWN_darwin_to_fbsd(fbsd, darwin)           if (flags & (darwin)) ret |= (fbsd);

#define DECL_FBSD_constant(const_name, val, ...) const_name = val,
#define DECL_ALIAS_constant(const_name, val)      const_name = val,
#define FROM_DARWN_constant(const_name, val)

#define DECL_FBSD_strtable(const_name, val, ...) case val: return #const_name;
#define DECL_ALIAS_strtable(const_name, val)
#define FROM_DARWN_strtable(const_name, val)

#define DECL_FBSD(tag, const_name, val, ...) DECL_FBSD_ ## tag (FBSD_ ## const_name, val, ##__VA_ARGS__, const_name)
#define DECL_ALIAS(tag, const_name, val)      DECL_ALIAS_ ## tag (FBSD_ ## const_name, val)
#define FROM_DARWN(tag, const_name, val)      FROM_DARWN_ ## tag (FBSD_ ## const_name, val)

#define FBSD_SPECIFIC (-10 -__COUNTER__)  // Unique value that does not conflict with any fbsd constants


#define DECLARE_CSTR_FUNC(const_id, const_list) \
  static inline char * fbsd_##const_id##_##str(int64_t val) {\
    switch (val) { \
      const_list(strtable)\
    }\
    return "(No " #const_id " Matched)";\
  }

__private_extern__ void
__ufs_debug(thread_t, const char*, int, const char*, const char*, ...) __printflike(5, 6);

#pragma mark Convert bulk flags. YOU NEED TO DECLARE 'convert_func' WITH DECLARE_CMAP_FUNC() FIRST.
#define DECLARE_MULTIFLAG_CMAP_FUNC(tag, const_id, const_list)                              \
    static inline int64_t tag ## _ ## const_id (int64_t _flags, uint64_t supported_mask) {  \
        uint64_t ret = 0;                                                                   \
        uint64_t flags = _flags & supported_mask;                                           \
        const_list(tag)                                                                     \
        __ufs_debug(current_thread(), __FILE__, __LINE__, __func__,                         \
                    #const_id " is converted to: 0x%llx\n", ret);                           \
        return ret;                                                                         \
    }

#define DECLARE_CENUM(const_id, const_list) \
  enum fbsd_##const_id { \
    const_list(constant)\
  };


#endif /* util_h */
