#pragma once
// OpenStone's little-endian C11/C++ port. Upstream arithmetic sources are unchanged.
#define LITTLEENDIAN 1
#define INLINE static inline
#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#elif defined(__cplusplus)
#define THREAD_LOCAL thread_local
#else
#define THREAD_LOCAL _Thread_local
#endif
#if (defined(__GNUC__) || defined(__clang__)) && !defined(OPENSTONE_SOFTFLOAT_PORTABLE)
#define SOFTFLOAT_BUILTIN_CLZ 1
#if defined(__SIZEOF_INT128__)
#define SOFTFLOAT_INTRINSIC_INT128 1
#endif
#include "opts-GCC.h"
#endif
