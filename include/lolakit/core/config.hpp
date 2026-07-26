#ifndef LOLAKIT_CORE_CONFIG_HPP_
#define LOLAKIT_CORE_CONFIG_HPP_

#include <cstddef>

#if defined(_MSC_VER)
#define LOLAKIT_COMPILER_MSVC 1
#else
#define LOLAKIT_COMPILER_MSVC 0
#endif

#if defined(__clang__)
#define LOLAKIT_COMPILER_CLANG 1
#else
#define LOLAKIT_COMPILER_CLANG 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define LOLAKIT_COMPILER_GCC 1
#else
#define LOLAKIT_COMPILER_GCC 0
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define LOLAKIT_ARCH_X86_64 1
#else
#define LOLAKIT_ARCH_X86_64 0
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
#define LOLAKIT_ARCH_AARCH64 1
#else
#define LOLAKIT_ARCH_AARCH64 0
#endif

#if defined(__has_builtin)
#define LOLAKIT_HAS_BUILTIN(x) __has_builtin(x)
#else
#define LOLAKIT_HAS_BUILTIN(x) 0
#endif

#define LOLAKIT_CACHE_LINE_SIZE 64

namespace lolakit {
namespace core {

inline constexpr std::size_t kCacheLineSize = LOLAKIT_CACHE_LINE_SIZE;

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_CONFIG_HPP_
