#ifndef LOLAKIT_CORE_MACROS_HPP_
#define LOLAKIT_CORE_MACROS_HPP_

#include "lolakit/core/config.hpp"

#if LOLAKIT_COMPILER_MSVC
#define LOLAKIT_FORCE_INLINE __forceinline
#define LOLAKIT_NOINLINE __declspec(noinline)
#define LOLAKIT_ASSUME(expr) __assume(expr)
#elif LOLAKIT_COMPILER_CLANG || LOLAKIT_COMPILER_GCC
#define LOLAKIT_FORCE_INLINE inline __attribute__((always_inline))
#define LOLAKIT_NOINLINE __attribute__((noinline))
#define LOLAKIT_ASSUME(expr)                                    \
  do {                                                          \
    if (!(expr)) {                                              \
      __builtin_unreachable();                                  \
    }                                                           \
  } while (false)
#else
#define LOLAKIT_FORCE_INLINE inline
#define LOLAKIT_NOINLINE
#define LOLAKIT_ASSUME(expr) ((void) 0)
#endif

#if LOLAKIT_COMPILER_CLANG || LOLAKIT_COMPILER_GCC
#define LOLAKIT_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define LOLAKIT_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define LOLAKIT_PREFETCH(addr) __builtin_prefetch((addr))
#define LOLAKIT_UNREACHABLE() __builtin_unreachable()
#else
#define LOLAKIT_LIKELY(expr) (expr)
#define LOLAKIT_UNLIKELY(expr) (expr)
#define LOLAKIT_PREFETCH(addr) ((void) (addr))
#define LOLAKIT_UNREACHABLE() ((void) 0)
#endif

#endif  // LOLAKIT_CORE_MACROS_HPP_
