#ifndef LOLAKIT_CORE_SHARED_MEMORY_QUEUE_HPP_
#define LOLAKIT_CORE_SHARED_MEMORY_QUEUE_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "lolakit/core/atomic.hpp"
#include "lolakit/core/cacheline.hpp"
#include "lolakit/core/config.hpp"
#include "lolakit/core/spin_wait.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lolakit {
namespace core {

template <std::size_t PayloadSize, std::size_t Capacity>
class SharedMemorySpscQueue {
 public:
  static_assert(PayloadSize > 0U, "PayloadSize must be greater than zero");
  static_assert(Capacity >= 2U, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1U)) == 0U,
                "Capacity must be a power of two");
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "SharedMemorySpscQueue requires lock-free std::atomic<size_t>");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "SharedMemorySpscQueue requires lock-free std::atomic<uint32_t>");

  enum class OpenMode {
    kCreateOnly,
    kOpenOnly,
    kOpenOrCreate,
  };

  SharedMemorySpscQueue() = default;

  ~SharedMemorySpscQueue() { close(); }

  SharedMemorySpscQueue(const SharedMemorySpscQueue&) = delete;
  SharedMemorySpscQueue& operator=(const SharedMemorySpscQueue&) = delete;

  SharedMemorySpscQueue(SharedMemorySpscQueue&& other) noexcept {
    move_from(std::move(other));
  }

  SharedMemorySpscQueue& operator=(SharedMemorySpscQueue&& other) noexcept {
    if (this != &other) {
      close();
      move_from(std::move(other));
    }
    return *this;
  }

  static SharedMemorySpscQueue create(const std::string& name) noexcept {
    return open(name, OpenMode::kCreateOnly);
  }

  static SharedMemorySpscQueue attach(const std::string& name) noexcept {
    return open(name, OpenMode::kOpenOnly);
  }

  static SharedMemorySpscQueue open(const std::string& name,
                                    OpenMode mode = OpenMode::kOpenOrCreate) noexcept {
    SharedMemorySpscQueue queue;
    queue.open_internal(name, mode);
    return queue;
  }

  bool is_open() const noexcept { return header_ != nullptr; }

  bool was_created() const noexcept { return created_; }

  std::uint32_t last_error() const noexcept { return last_error_; }

  static constexpr std::size_t payload_size() noexcept { return PayloadSize; }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool empty() const noexcept {
#if defined(_WIN32)
    return atomic_load_acquire(header_->producer_index.value()) ==
           atomic_load_acquire(header_->consumer_index.value());
#else
    (void)header_;
    return true;
#endif
  }

  bool full() noexcept {
#if defined(_WIN32)
    const std::size_t producer = atomic_load_relaxed(header_->producer_index.value());
    std::size_t consumer_cache = producer_cached_consumer_;

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(header_->consumer_index.value());
      producer_cached_consumer_ = consumer_cache;
    }

    return producer - consumer_cache == Capacity;
#else
    (void)producer_cached_consumer_;
    return true;
#endif
  }

  std::size_t size() const noexcept {
#if defined(_WIN32)
    const std::size_t producer = atomic_load_acquire(header_->producer_index.value());
    const std::size_t consumer = atomic_load_acquire(header_->consumer_index.value());
    return producer - consumer;
#else
    (void)header_;
    return 0U;
#endif
  }

  bool try_push(const void* data, std::uint32_t size) noexcept {
#if defined(_WIN32)
    if (data == nullptr || size == 0U || size > PayloadSize || !is_open()) {
      return false;
    }

    const std::size_t producer = atomic_load_relaxed(header_->producer_index.value());
    std::size_t consumer_cache = producer_cached_consumer_;

    if (producer - consumer_cache == Capacity) {
      consumer_cache = atomic_load_acquire(header_->consumer_index.value());
      producer_cached_consumer_ = consumer_cache;
      if (producer - consumer_cache == Capacity) {
        return false;
      }
    }

    Slot& slot = slots_[producer & kIndexMask];
    std::memcpy(slot.payload, data, size);
    slot.size = size;
    atomic_store_release(header_->producer_index.value(), producer + 1U);
    return true;
#else
    (void)data;
    (void)size;
    (void)producer_cached_consumer_;
    last_error_ = kErrorNotSupported;
    return false;
#endif
  }

  template <typename T>
  bool try_push(const T& value) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SharedMemorySpscQueue::try_push<T> requires trivially copyable T");
    static_assert(sizeof(T) <= PayloadSize,
                  "SharedMemorySpscQueue::try_push<T> payload exceeds PayloadSize");
    return try_push(&value, static_cast<std::uint32_t>(sizeof(T)));
  }

  bool try_pop(void* out, std::uint32_t out_capacity, std::uint32_t& size) noexcept {
    size = 0U;
#if defined(_WIN32)
    if (out == nullptr || !is_open()) {
      return false;
    }

    const std::size_t consumer = atomic_load_relaxed(header_->consumer_index.value());
    std::size_t producer_cache = consumer_cached_producer_;

    if (consumer == producer_cache) {
      producer_cache = atomic_load_acquire(header_->producer_index.value());
      consumer_cached_producer_ = producer_cache;
      if (consumer == producer_cache) {
        return false;
      }
    }

    Slot& slot = slots_[consumer & kIndexMask];
    if (out_capacity < slot.size) {
      size = slot.size;
      return false;
    }

    std::memcpy(out, slot.payload, slot.size);
    size = slot.size;
    atomic_store_release(header_->consumer_index.value(), consumer + 1U);
    return true;
#else
    (void)out;
    (void)out_capacity;
    (void)consumer_cached_producer_;
    last_error_ = kErrorNotSupported;
    return false;
#endif
  }

  template <typename T>
  bool try_pop(T& value) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SharedMemorySpscQueue::try_pop<T> requires trivially copyable T");

    std::uint32_t size = 0U;
    if (!try_pop(&value, static_cast<std::uint32_t>(sizeof(T)), size)) {
      return false;
    }
    return size == sizeof(T);
  }

  void close() noexcept {
#if defined(_WIN32)
    if (view_ != nullptr) {
      ::UnmapViewOfFile(view_);
      view_ = nullptr;
    }
    if (mapping_handle_ != nullptr) {
      ::CloseHandle(mapping_handle_);
      mapping_handle_ = nullptr;
    }

    header_ = nullptr;
    slots_ = nullptr;
    created_ = false;
    producer_cached_consumer_ = 0U;
    consumer_cached_producer_ = 0U;
#else
    header_ = nullptr;
    slots_ = nullptr;
    created_ = false;
    producer_cached_consumer_ = 0U;
    consumer_cached_producer_ = 0U;
#endif
  }

 private:
#if defined(_WIN32)
  struct LayoutHeader {
    std::uint32_t magic{0U};
    std::uint32_t version{0U};
    std::uint32_t payload_size{0U};
    std::uint32_t capacity{0U};
    CacheLinePadded<std::atomic<std::uint32_t>> init_state{0U};
    CacheLinePadded<std::atomic<std::size_t>> producer_index{0U};
    CacheLinePadded<std::atomic<std::size_t>> consumer_index{0U};
  };

  struct Slot {
    std::uint32_t size{0U};
    alignas(kCacheLineSize) unsigned char payload[PayloadSize];
  };

  static constexpr std::size_t kIndexMask = Capacity - 1U;
  static constexpr std::uint32_t kMagic = 0x4C4F4C41U;
  static constexpr std::uint32_t kVersion = 1U;
  static constexpr std::uint32_t kInitInProgress = 1U;
  static constexpr std::uint32_t kInitReady = 2U;

  static std::wstring make_mapping_name(const std::string& name) {
    const std::string prefix = "Local\\LoLaKit.";
    const std::string full_name = prefix + name;
    return std::wstring(full_name.begin(), full_name.end());
  }

  static std::size_t mapping_size() noexcept {
    return sizeof(LayoutHeader) + sizeof(Slot) * Capacity;
  }

  void move_from(SharedMemorySpscQueue&& other) noexcept {
    mapping_handle_ = other.mapping_handle_;
    view_ = other.view_;
    header_ = other.header_;
    slots_ = other.slots_;
    created_ = other.created_;
    last_error_ = other.last_error_;
    producer_cached_consumer_ = other.producer_cached_consumer_;
    consumer_cached_producer_ = other.consumer_cached_producer_;

    other.mapping_handle_ = nullptr;
    other.view_ = nullptr;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.created_ = false;
    other.last_error_ = 0U;
    other.producer_cached_consumer_ = 0U;
    other.consumer_cached_producer_ = 0U;
  }

  void open_internal(const std::string& name, OpenMode mode) noexcept {
    close();

    const std::wstring mapping_name = make_mapping_name(name);
    HANDLE mapping = nullptr;
    bool created = false;

    switch (mode) {
      case OpenMode::kCreateOnly:
        mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0U, static_cast<DWORD>(mapping_size()),
                                       mapping_name.c_str());
        if (mapping != nullptr) {
          created = ::GetLastError() != ERROR_ALREADY_EXISTS;
          if (!created) {
            last_error_ = ERROR_ALREADY_EXISTS;
            ::CloseHandle(mapping);
            return;
          }
        }
        break;
      case OpenMode::kOpenOnly:
        mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str());
        break;
      case OpenMode::kOpenOrCreate:
        mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0U, static_cast<DWORD>(mapping_size()),
                                       mapping_name.c_str());
        if (mapping != nullptr) {
          created = ::GetLastError() != ERROR_ALREADY_EXISTS;
        }
        break;
    }

    if (mapping == nullptr) {
      last_error_ = ::GetLastError();
      return;
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0U, 0U, mapping_size());
    if (view == nullptr) {
      last_error_ = ::GetLastError();
      ::CloseHandle(mapping);
      return;
    }

    mapping_handle_ = mapping;
    view_ = view;
    header_ = static_cast<LayoutHeader*>(view_);
    slots_ = reinterpret_cast<Slot*>(static_cast<unsigned char*>(view_) +
                                     sizeof(LayoutHeader));
    created_ = created;
    producer_cached_consumer_ = 0U;
    consumer_cached_producer_ = 0U;
    last_error_ = 0U;

    if (created) {
      initialize_layout();
      return;
    }

    wait_until_ready();
  }

  void initialize_layout() noexcept {
    std::memset(view_, 0, mapping_size());
    header_->init_state.value().store(kInitInProgress, std::memory_order_relaxed);
    header_->magic = kMagic;
    header_->version = kVersion;
    header_->payload_size = static_cast<std::uint32_t>(PayloadSize);
    header_->capacity = static_cast<std::uint32_t>(Capacity);
    header_->producer_index.value().store(0U, std::memory_order_relaxed);
    header_->consumer_index.value().store(0U, std::memory_order_relaxed);
    header_->init_state.value().store(kInitReady, std::memory_order_release);
  }

  void wait_until_ready() noexcept {
    SpinWait wait;
    while (header_->init_state.value().load(std::memory_order_acquire) != kInitReady) {
      wait.pause();
    }

    if (header_->magic != kMagic || header_->version != kVersion ||
        header_->payload_size != PayloadSize || header_->capacity != Capacity) {
      last_error_ = ERROR_INVALID_DATA;
      close();
    }
  }

  HANDLE mapping_handle_{nullptr};
  void* view_{nullptr};
  LayoutHeader* header_{nullptr};
  Slot* slots_{nullptr};
  bool created_{false};
  std::uint32_t last_error_{0U};
  std::size_t producer_cached_consumer_{0U};
  std::size_t consumer_cached_producer_{0U};
#else
  static constexpr std::uint32_t kErrorNotSupported = 0xFFFFFFFFU;

  struct LayoutHeader;
  struct Slot;

  void move_from(SharedMemorySpscQueue&& other) noexcept {
    header_ = other.header_;
    slots_ = other.slots_;
    created_ = other.created_;
    last_error_ = other.last_error_;
    producer_cached_consumer_ = other.producer_cached_consumer_;
    consumer_cached_producer_ = other.consumer_cached_producer_;

    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.created_ = false;
    other.last_error_ = 0U;
    other.producer_cached_consumer_ = 0U;
    other.consumer_cached_producer_ = 0U;
  }

  void open_internal(const std::string& name, OpenMode mode) noexcept {
    (void)name;
    (void)mode;
    close();
    last_error_ = kErrorNotSupported;
  }

  LayoutHeader* header_{nullptr};
  Slot* slots_{nullptr};
  bool created_{false};
  std::uint32_t last_error_{0U};
  std::size_t producer_cached_consumer_{0U};
  std::size_t consumer_cached_producer_{0U};
#endif
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_SHARED_MEMORY_QUEUE_HPP_
