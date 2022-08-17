/*-
 *  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or
 *  code or tables extracted from it, as desired without restriction.
 *
 * $FreeBSD$
 */

#ifndef _SYS_GSB_CRC32_H_
#define _SYS_GSB_CRC32_H_

#include <libkern/libkern.h>
#include <sys/types.h>

uint32_t calculate_crc32c(uint32_t crc32c, const unsigned char *buffer, unsigned int length);

#endif /* !_SYS_GSB_CRC32_H_ */
