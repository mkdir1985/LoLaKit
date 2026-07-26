#ifndef LOLAKIT_CORE_TSC_CLOCK_HPP_
#define LOLAKIT_CORE_TSC_CLOCK_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "lolakit/core/assert.hpp"
#include "lolakit/core/config.hpp"
#include "lolakit/core/spin_wait.hpp"

#if LOLAKIT_COMPILER_MSVC
#include <intrin.h>
#endif

#if LOLAKIT_ARCH_X86_64
#include <immintrin.h>
#endif

namespace lolakit {
namespace core {

class TscClock {
 public:
  static bool is_native() noexcept {
#if LOLAKIT_ARCH_X86_64
    return true;
#else
    return false;
#endif
  }

  static void initialize(
      std::chrono::milliseconds sample_window = std::chrono::milliseconds(20)) {
    if (sample_window <= std::chrono::milliseconds::zero()) {
      sample_window = std::chrono::milliseconds(1);
    }

    std::call_once(state().once, [sample_window]() {
      Calibration calibration{};

#if LOLAKIT_ARCH_X86_64
      calibration.native = true;

      const auto start_time = std::chrono::steady_clock::now();
      const std::uint64_t start_cycles = read_native_cycles();
      const auto deadline = start_time + sample_window;

      SpinWait spin_wait;
      while (std::chrono::steady_clock::now() < deadline) {
        spin_wait.pause();
      }

      const auto end_time = std::chrono::steady_clock::now();
      const std::uint64_t end_cycles = read_native_cycles();
      const std::uint64_t elapsed_ns =
          duration_to_ns(end_time.time_since_epoch()) -
          duration_to_ns(start_time.time_since_epoch());
      const std::uint64_t elapsed_cycles = end_cycles - start_cycles;

      LOLAKIT_ASSERT(elapsed_ns > 0U);
      LOLAKIT_ASSERT(elapsed_cycles > 0U);

      calibration.base_cycles = end_cycles;
      calibration.base_ns = duration_to_ns(end_time.time_since_epoch());
      calibration.cycles_per_ns =
          static_cast<double>(elapsed_cycles) / static_cast<double>(elapsed_ns);
      calibration.ns_per_cycle =
          static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_cycles);
#else
      calibration.native = false;
      calibration.base_ns =
          duration_to_ns(std::chrono::steady_clock::now().time_since_epoch());
      calibration.base_cycles = calibration.base_ns;
      calibration.cycles_per_ns = 1.0;
      calibration.ns_per_cycle = 1.0;
#endif

      state().calibration = calibration;
      state().initialized.store(true, std::memory_order_release);
    });
  }

  static bool is_initialized() noexcept {
    return state().initialized.load(std::memory_order_acquire);
  }

  static std::uint64_t now_cycles() noexcept {
    if (!is_initialized()) {
      initialize();
    }

#if LOLAKIT_ARCH_X86_64
    return read_native_cycles();
#else
    return duration_to_ns(std::chrono::steady_clock::now().time_since_epoch());
#endif
  }

  static std::uint64_t now_ns() noexcept {
    if (!is_initialized()) {
      initialize();
    }

    const Calibration& calibration = state().calibration;
    if (!calibration.native) {
      return duration_to_ns(std::chrono::steady_clock::now().time_since_epoch());
    }

    const std::uint64_t cycles = read_native_cycles();
    const std::uint64_t delta_cycles = cycles - calibration.base_cycles;
    const auto delta_ns = static_cast<std::uint64_t>(
        static_cast<double>(delta_cycles) * calibration.ns_per_cycle);
    return calibration.base_ns + delta_ns;
  }

  static double cycles_per_ns() noexcept {
    if (!is_initialized()) {
      initialize();
    }
    return state().calibration.cycles_per_ns;
  }

  static double ns_per_cycle() noexcept {
    if (!is_initialized()) {
      initialize();
    }
    return state().calibration.ns_per_cycle;
  }

 private:
  struct Calibration {
    bool native{false};
    std::uint64_t base_cycles{0};
    std::uint64_t base_ns{0};
    double cycles_per_ns{1.0};
    double ns_per_cycle{1.0};
  };

  struct State {
    std::once_flag once;
    Calibration calibration{};
    std::atomic<bool> initialized{false};
  };

  static State& state() noexcept {
    static State value;
    return value;
  }

  template <typename Rep, typename Period>
  static std::uint64_t duration_to_ns(
      const std::chrono::duration<Rep, Period>& duration) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
  }

#if LOLAKIT_ARCH_X86_64
  static std::uint64_t read_native_cycles() noexcept {
#if LOLAKIT_COMPILER_MSVC
    _mm_lfence();
    return __rdtsc();
#elif LOLAKIT_COMPILER_CLANG || LOLAKIT_COMPILER_GCC
    _mm_lfence();
    return __rdtsc();
#else
    return 0U;
#endif
  }
#endif
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_TSC_CLOCK_HPP_
