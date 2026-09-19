#ifndef LOLAKIT_CORE_VECTOR_SPSC_RING_BUFFER_HPP_
#define LOLAKIT_CORE_VECTOR_SPSC_RING_BUFFER_HPP_

#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "lolakit/core/assert.hpp"
#include "lolakit/core/atomic.hpp"
#include "lolakit/core/cacheline.hpp"
#include "lolakit/core/noncopyable.hpp"

namespace lolakit {
namespace core {

template <typename T, std::size_t Capacity>
class VectorSpscRingBuffer : private NonCopyableNonMovable {
 public:
  static_assert(Capacity >= 2U, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "Capacity must be a power of two");

  VectorSpscRingBuffer() : slots_(Capacity) {}

  ~VectorSpscRingBuffer() { clear(); }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool empty() const noexcept {
    return atomic_load_acquire(producer_index_.value()) ==
           atomic_load_acquire(consumer_index_.value());
  }

  bool full() noexcept {
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());
    std::size_t consumer_cache = producer_cached_consumer_.value();

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(consumer_index_.value());
      producer_cached_consumer_.value() = consumer_cache;
    }

    return producer - consumer_cache == Capacity;
  }

  std::size_t size() const noexcept {
    const std::size_t producer = atomic_load_acquire(producer_index_.value());
    const std::size_t consumer = atomic_load_acquire(consumer_index_.value());
    return producer - consumer;
  }

  bool try_push(const T& value) { return emplace(value); }

  bool try_push(T&& value) { return emplace(std::move(value)); }

  template <typename... Args>
  bool emplace(Args&&... args) {
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());
    std::size_t consumer_cache = producer_cached_consumer_.value();

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(consumer_index_.value());
      producer_cached_consumer_.value() = consumer_cache;
      if (producer - consumer_cache == Capacity) {
        return false;
      }
    }

    Slot& slot = slots_[producer & kIndexMask];
    ::new (static_cast<void*>(slot.storage)) T(std::forward<Args>(args)...);
    atomic_store_release(producer_index_.value(), producer + 1U);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t consumer = atomic_load_relaxed(consumer_index_.value());
    std::size_t producer_cache = consumer_cached_producer_.value();

    if (consumer == producer_cache) {
      producer_cache = atomic_load_acquire(producer_index_.value());
      consumer_cached_producer_.value() = producer_cache;
      if (consumer == producer_cache) {
        return false;
      }
    }

    Slot& slot = slots_[consumer & kIndexMask];
    T* value = slot.ptr();
    out = std::move(*value);
    value->~T();
    atomic_store_release(consumer_index_.value(), consumer + 1U);
    return true;
  }

  std::optional<T> try_pop() {
    const std::size_t consumer = atomic_load_relaxed(consumer_index_.value());
    std::size_t producer_cache = consumer_cached_producer_.value();

    if (consumer == producer_cache) {
      producer_cache = atomic_load_acquire(producer_index_.value());
      consumer_cached_producer_.value() = producer_cache;
      if (consumer == producer_cache) {
        return std::nullopt;
      }
    }

    Slot& slot = slots_[consumer & kIndexMask];
    T* value = slot.ptr();
    std::optional<T> result(std::move(*value));
    value->~T();
    atomic_store_release(consumer_index_.value(), consumer + 1U);
    return result;
  }

  void clear() noexcept {
    std::size_t consumer = atomic_load_relaxed(consumer_index_.value());
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());

    while (consumer != producer) {
      Slot& slot = slots_[consumer & kIndexMask];
      destroy_slot(slot);
      ++consumer;
    }

    atomic_store_relaxed(consumer_index_.value(), producer);
    atomic_store_relaxed(producer_index_.value(), producer);
    producer_cached_consumer_.value() = producer;
    consumer_cached_producer_.value() = producer;
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

  CacheLinePadded<std::atomic<std::size_t>> producer_index_{0U};
  CacheLinePadded<std::size_t> producer_cached_consumer_{0U};
  CacheLinePadded<std::atomic<std::size_t>> consumer_index_{0U};
  CacheLinePadded<std::size_t> consumer_cached_producer_{0U};
  std::vector<Slot> slots_;
};

template <typename T, std::size_t Capacity>
class TrivialVectorSpscRingBuffer : private NonCopyableNonMovable {
 public:
  static_assert(Capacity >= 2U, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "TrivialVectorSpscRingBuffer requires trivially copyable T");
  static_assert(std::is_trivially_destructible<T>::value,
                "TrivialVectorSpscRingBuffer requires trivially destructible T");
  static_assert(std::is_trivially_default_constructible<T>::value,
                "TrivialVectorSpscRingBuffer requires trivially default constructible T");

  TrivialVectorSpscRingBuffer() : slots_(Capacity, T{}) {}
  ~TrivialVectorSpscRingBuffer() = default;

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool empty() const noexcept {
    return atomic_load_acquire(producer_index_.value()) ==
           atomic_load_acquire(consumer_index_.value());
  }

  bool full() noexcept {
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());
    std::size_t consumer_cache = producer_cached_consumer_.value();

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(consumer_index_.value());
      producer_cached_consumer_.value() = consumer_cache;
    }

    return producer - consumer_cache == Capacity;
  }

  std::size_t size() const noexcept {
    const std::size_t producer = atomic_load_acquire(producer_index_.value());
    const std::size_t consumer = atomic_load_acquire(consumer_index_.value());
    return producer - consumer;
  }

  bool try_push(const T& value) noexcept {
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());
    std::size_t consumer_cache = producer_cached_consumer_.value();

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(consumer_index_.value());
      producer_cached_consumer_.value() = consumer_cache;
      if (producer - consumer_cache == Capacity) {
        return false;
      }
    }

    slots_[producer & kIndexMask] = value;
    atomic_store_release(producer_index_.value(), producer + 1U);
    return true;
  }

  bool try_push(T&& value) noexcept {
    return try_push(static_cast<const T&>(value));
  }

  template <typename... Args>
  bool emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args...>::value) {
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());
    std::size_t consumer_cache = producer_cached_consumer_.value();

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(consumer_index_.value());
      producer_cached_consumer_.value() = consumer_cache;
      if (producer - consumer_cache == Capacity) {
        return false;
      }
    }

    slots_[producer & kIndexMask] = T(std::forward<Args>(args)...);
    atomic_store_release(producer_index_.value(), producer + 1U);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::size_t consumer = atomic_load_relaxed(consumer_index_.value());
    std::size_t producer_cache = consumer_cached_producer_.value();

    if (consumer == producer_cache) {
      producer_cache = atomic_load_acquire(producer_index_.value());
      consumer_cached_producer_.value() = producer_cache;
      if (consumer == producer_cache) {
        return false;
      }
    }

    out = slots_[consumer & kIndexMask];
    atomic_store_release(consumer_index_.value(), consumer + 1U);
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
    const std::size_t producer = atomic_load_relaxed(producer_index_.value());
    atomic_store_relaxed(consumer_index_.value(), producer);
    atomic_store_relaxed(producer_index_.value(), producer);
    producer_cached_consumer_.value() = producer;
    consumer_cached_producer_.value() = producer;
  }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1U;

  CacheLinePadded<std::atomic<std::size_t>> producer_index_{0U};
  CacheLinePadded<std::size_t> producer_cached_consumer_{0U};
  CacheLinePadded<std::atomic<std::size_t>> consumer_index_{0U};
  CacheLinePadded<std::size_t> consumer_cached_producer_{0U};
  std::vector<T> slots_;
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_VECTOR_SPSC_RING_BUFFER_HPP_
