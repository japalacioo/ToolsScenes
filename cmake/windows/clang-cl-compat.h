/* Solo para compilación cruzada con clang-cl: clang no implementa el
 * intrínseco _udiv128 de MSVC que usa libobs/util/util_uint64.h. */
#pragma once
#if defined(__clang__) && defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
static inline unsigned __int64 scene_previo_udiv128(unsigned __int64 high, unsigned __int64 low,
						    unsigned __int64 divisor, unsigned __int64 *remainder)
{
	unsigned __int64 quotient, rem;
	__asm__("divq %4" : "=a"(quotient), "=d"(rem) : "a"(low), "d"(high), "rm"(divisor));
	*remainder = rem;
	return quotient;
}
#define _udiv128 scene_previo_udiv128
#endif
