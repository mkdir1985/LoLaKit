#ifndef LOLAKIT_CORE_ASSERT_HPP_
#define LOLAKIT_CORE_ASSERT_HPP_

#include <cstdio>
#include <cstdlib>

#include "lolakit/core/config.hpp"

#if LOLAKIT_COMPILER_MSVC
#include <intrin.h>
#endif

namespace lolakit {
namespace core {
namespace detail {

[[noreturn]] inline void assert_fail(const char* expr, const char* file,
                                     int line) noexcept {
  std::fprintf(stderr, "[lolakit] assertion failed: %s (%s:%d)\n", expr, file,
               line);
  std::fflush(stderr);

#if LOLAKIT_COMPILER_MSVC
  __debugbreak();
#elif LOLAKIT_COMPILER_CLANG || LOLAKIT_COMPILER_GCC
  __builtin_trap();
#else
  std::abort();
#endif

  std::abort();
}

}  // namespace detail
}  // namespace core
}  // namespace lolakit

#ifndef NDEBUG
#define LOLAKIT_ASSERT(expr)                                              \
  do {                                                                    \
    if (!(expr)) {                                                        \
      ::lolakit::core::detail::assert_fail(#expr, __FILE__, __LINE__);    \
    }                                                                     \
  } while (false)
#else
#define LOLAKIT_ASSERT(expr) ((void) sizeof(expr))
#endif

#define LOLAKIT_DCHECK(expr) LOLAKIT_ASSERT(expr)

#endif  // LOLAKIT_CORE_ASSERT_HPP_
