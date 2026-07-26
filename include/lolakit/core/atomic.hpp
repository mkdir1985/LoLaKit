#ifndef LOLAKIT_CORE_ATOMIC_HPP_
#define LOLAKIT_CORE_ATOMIC_HPP_

#include <atomic>

#include "lolakit/core/macros.hpp"

namespace lolakit {
namespace core {

template <typename T>
using Atomic = std::atomic<T>;

template <typename T>
LOLAKIT_FORCE_INLINE T atomic_load_relaxed(
    const std::atomic<T>& value) noexcept {
  return value.load(std::memory_order_relaxed);
}

template <typename T>
LOLAKIT_FORCE_INLINE T atomic_load_acquire(
    const std::atomic<T>& value) noexcept {
  return value.load(std::memory_order_acquire);
}

template <typename T>
LOLAKIT_FORCE_INLINE void atomic_store_relaxed(std::atomic<T>& value,
                                               T desired) noexcept {
  value.store(desired, std::memory_order_relaxed);
}

template <typename T>
LOLAKIT_FORCE_INLINE void atomic_store_release(std::atomic<T>& value,
                                               T desired) noexcept {
  value.store(desired, std::memory_order_release);
}

template <typename T>
LOLAKIT_FORCE_INLINE T atomic_exchange_acq_rel(std::atomic<T>& value,
                                               T desired) noexcept {
  return value.exchange(desired, std::memory_order_acq_rel);
}

template <typename T>
LOLAKIT_FORCE_INLINE bool atomic_compare_exchange_weak_acq_rel(
    std::atomic<T>& value, T& expected, T desired) noexcept {
  return value.compare_exchange_weak(expected, desired,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire);
}

template <typename T>
LOLAKIT_FORCE_INLINE bool atomic_compare_exchange_strong_acq_rel(
    std::atomic<T>& value, T& expected, T desired) noexcept {
  return value.compare_exchange_strong(expected, desired,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
}

template <typename T>
LOLAKIT_FORCE_INLINE T atomic_fetch_add_relaxed(std::atomic<T>& value,
                                                T increment) noexcept {
  return value.fetch_add(increment, std::memory_order_relaxed);
}

template <typename T>
LOLAKIT_FORCE_INLINE T atomic_fetch_sub_relaxed(std::atomic<T>& value,
                                                T decrement) noexcept {
  return value.fetch_sub(decrement, std::memory_order_relaxed);
}

LOLAKIT_FORCE_INLINE void atomic_thread_fence_acquire() noexcept {
  std::atomic_thread_fence(std::memory_order_acquire);
}

LOLAKIT_FORCE_INLINE void atomic_thread_fence_release() noexcept {
  std::atomic_thread_fence(std::memory_order_release);
}

LOLAKIT_FORCE_INLINE void atomic_thread_fence_seq_cst() noexcept {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_ATOMIC_HPP_
