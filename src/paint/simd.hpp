#pragma once

// Explicit SSE2 code paths in the paint module are compiled only where the
// compiler guarantees SSE2: GCC and Clang targeting it (the x86-64
// default), MSVC for x64 (except ARM64EC) and for x86 with /arch:SSE2 or
// higher. Every such path has a portable fallback with identical results.
#if defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC)) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define WUI_PAINT_SSE2 1
#include <emmintrin.h>
#endif
