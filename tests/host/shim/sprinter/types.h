/* Host-build stand-in for the SDK's <sprinter/types.h>.
 * Types must match SDCC z80 widths exactly: the checked sources
 * assemble multi-byte values byte-wise through u8* casts, so u32
 * must be exactly 32 bits (a 64-bit long would leave garbage in
 * the upper half). Host must be little-endian, like the Z80. */
#ifndef HOST_SPRINTER_TYPES_H
#define HOST_SPRINTER_TYPES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;

#endif
