#ifndef LOLAKIT_CORE_CACHELINE_HPP_
#define LOLAKIT_CORE_CACHELINE_HPP_

#include <cstddef>
#include <type_traits>
#include <utility>

#include "lolakit/core/config.hpp"

namespace lolakit {
namespace core {

template <std::size_t N>
struct CacheLinePadding {
  unsigned char bytes[N];
};

template <>
struct CacheLinePadding<0> {};

template <typename T>
class alignas(kCacheLineSize) CacheLinePadded {
 public:
  static_assert(std::is_object<T>::value, "T must be an object type");

  CacheLinePadded() = default;

  template <typename... Args>
  explicit CacheLinePadded(Args&&... args)
      : value_(std::forward<Args>(args)...) {}

  T& value() noexcept { return value_; }
  const T& value() const noexcept { return value_; }

  T* operator->() noexcept { return &value_; }
  const T* operator->() const noexcept { return &value_; }

  T& operator*() noexcept { return value_; }
  const T& operator*() const noexcept { return value_; }

 private:
  static constexpr std::size_t kPaddingSize =
      (kCacheLineSize - (sizeof(T) % kCacheLineSize)) % kCacheLineSize;
  static_assert((sizeof(T) + kPaddingSize) % kCacheLineSize == 0,
                "CacheLinePadded size must be cache-line aligned");

  T value_{};
  CacheLinePadding<kPaddingSize> padding_{};
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_CACHELINE_HPP_
