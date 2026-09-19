#ifndef LOLAKIT_CORE_MUTEX_VECTOR_SPSC_QUEUE_HPP_
#define LOLAKIT_CORE_MUTEX_VECTOR_SPSC_QUEUE_HPP_

#include <cstddef>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "lolakit/core/assert.hpp"
#include "lolakit/core/noncopyable.hpp"

namespace lolakit {
namespace core {

template <typename T, std::size_t Capacity>
class MutexVectorSpscQueue : private NonCopyableNonMovable {
 public:
  static_assert(Capacity >= 2U, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "Capacity must be a power of two");

  MutexVectorSpscQueue() : slots_(Capacity) {}

  ~MutexVectorSpscQueue() { clear(); }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_ == consumer_;
  }

  bool full() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_ - consumer_ == Capacity;
  }

  std::size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_ - consumer_;
  }

  bool try_push(const T& value) { return emplace(value); }

  bool try_push(T&& value) { return emplace(std::move(value)); }

  template <typename... Args>
  bool emplace(Args&&... args) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer_ - consumer_ == Capacity) {
      return false;
    }
    Slot& slot = slots_[producer_ & kIndexMask];
    ::new (static_cast<void*>(slot.storage)) T(std::forward<Args>(args)...);
    ++producer_;
    return true;
  }

  bool try_pop(T& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer_ == consumer_) {
      return false;
    }
    Slot& slot = slots_[consumer_ & kIndexMask];
    T* value = slot.ptr();
    out = std::move(*value);
    value->~T();
    ++consumer_;
    return true;
  }

  std::optional<T> try_pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer_ == consumer_) {
      return std::nullopt;
    }
    Slot& slot = slots_[consumer_ & kIndexMask];
    T* value = slot.ptr();
    std::optional<T> result(std::move(*value));
    value->~T();
    ++consumer_;
    return result;
  }

  void clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    while (consumer_ != producer_) {
      Slot& slot = slots_[consumer_ & kIndexMask];
      destroy_slot(slot);
      ++consumer_;
    }
    producer_ = consumer_;
  }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1U;

  struct Slot {
    alignas(T) unsigned char storage[sizeof(T)];

    T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
  };

  static void destroy_slot(Slot& slot) noexcept {
    if constexpr (!std::is_trivially_destructible<T>::value) {
      slot.ptr()->~T();
    }
  }

  mutable std::mutex mutex_;
  std::size_t producer_{0U};
  std::size_t consumer_{0U};
  std::vector<Slot> slots_;
};

template <typename T, std::size_t Capacity>
class TrivialMutexVectorSpscQueue : private NonCopyableNonMovable {
 public:
  static_assert(Capacity >= 2U, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "TrivialMutexVectorSpscQueue requires trivially copyable T");
  static_assert(std::is_trivially_destructible<T>::value,
                "TrivialMutexVectorSpscQueue requires trivially destructible T");
  static_assert(std::is_trivially_default_constructible<T>::value,
                "TrivialMutexVectorSpscQueue requires trivially default constructible T");

  TrivialMutexVectorSpscQueue() : slots_(Capacity, T{}) {}

  ~TrivialMutexVectorSpscQueue() = default;

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_ == consumer_;
  }

  bool full() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_ - consumer_ == Capacity;
  }

  std::size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_ - consumer_;
  }

  bool try_push(const T& value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer_ - consumer_ == Capacity) {
      return false;
    }
    slots_[producer_ & kIndexMask] = value;
    ++producer_;
    return true;
  }

  bool try_push(T&& value) noexcept {
    return try_push(static_cast<const T&>(value));
  }

  template <typename... Args>
  bool emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args...>::value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer_ - consumer_ == Capacity) {
      return false;
    }
    slots_[producer_ & kIndexMask] = T(std::forward<Args>(args)...);
    ++producer_;
    return true;
  }

  bool try_pop(T& out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer_ == consumer_) {
      return false;
    }
    out = slots_[consumer_ & kIndexMask];
    ++consumer_;
    return true;
  }

  std::optional<T> try_pop() noexcept {
    T value{};
    if (!try_pop(value)) {
      return std::nullopt;
    }
    return value;
  }

  void clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    consumer_ = producer_;
  }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1U;

  mutable std::mutex mutex_;
  std::size_t producer_{0U};
  std::size_t consumer_{0U};
  std::vector<T> slots_;
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_MUTEX_VECTOR_SPSC_QUEUE_HPP_
