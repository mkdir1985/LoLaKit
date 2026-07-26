#ifndef LOLAKIT_CORE_SPIN_WAIT_HPP_
#define LOLAKIT_CORE_SPIN_WAIT_HPP_

#include <atomic>
#include <cstdint>

#include "lolakit/core/config.hpp"
#include "lolakit/core/macros.hpp"

#if LOLAKIT_ARCH_X86_64
#include <immintrin.h>
#endif

namespace lolakit {
namespace core {

LOLAKIT_FORCE_INLINE void cpu_relax() noexcept {
#if LOLAKIT_ARCH_X86_64
  _mm_pause();
#elif LOLAKIT_ARCH_AARCH64 && (LOLAKIT_COMPILER_CLANG || LOLAKIT_COMPILER_GCC)
  __asm__ __volatile__("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

class SpinWait {
 public:
  void pause() noexcept {
    const std::uint32_t loops = iterations_ < 10U ? (1U << iterations_) : 1024U;
    for (std::uint32_t i = 0; i < loops; ++i) {
      cpu_relax();
    }

    if (iterations_ < 10U) {
      ++iterations_;
    }
  }

  void reset() noexcept { iterations_ = 0; }

 private:
  std::uint32_t iterations_{0};
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_SPIN_WAIT_HPP_
