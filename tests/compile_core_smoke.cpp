#include <cstdint>
#include <string_view>
#include <string>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "lolakit/core.hpp"

namespace {

class DemoSingleton final : public lolakit::core::Singleton<DemoSingleton> {
  friend class lolakit::core::Singleton<DemoSingleton>;

 public:
  std::uint64_t value() const noexcept { return value_; }

 private:
  DemoSingleton() : value_(42U) {}

  std::uint64_t value_;
};

class DemoNonCopyable final : private lolakit::core::NonCopyable {
 public:
  int value() const noexcept { return 7; }
};

std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

}  // namespace

int main() {
  auto& singleton = DemoSingleton::instance();
  lolakit::core::Atomic<std::uint64_t> counter(0U);
  lolakit::core::CacheLinePadded<std::uint64_t> padded(123U);
  lolakit::core::SpinWait spin_wait;
  lolakit::core::SpscRingBuffer<std::string, 8> queue;
  lolakit::core::TrivialSpscRingBuffer<std::uint64_t, 8> trivial_queue;
  using DemoSharedQueue = lolakit::core::SharedMemorySpscQueue<64, 8>;
  DemoNonCopyable non_copyable;
  const std::string mapping_name = "smoke." + std::to_string(current_process_id());
  auto shm_producer = DemoSharedQueue::open(mapping_name);
  auto shm_consumer = DemoSharedQueue::attach(mapping_name);

  static_assert(!std::is_copy_constructible<DemoSingleton>::value,
                "Singleton types should not be copy constructible");
  static_assert(!std::is_copy_constructible<DemoNonCopyable>::value,
                "NonCopyable should disable copy construction");
  static_assert(sizeof(decltype(padded)) % lolakit::core::kCacheLineSize == 0,
                "CacheLinePadded should be cache-line sized");
  static_assert(decltype(queue)::capacity() == 8U,
                "SpscRingBuffer capacity should match the template argument");
  static_assert(decltype(trivial_queue)::capacity() == 8U,
                "TrivialSpscRingBuffer capacity should match the template argument");
  static_assert(DemoSharedQueue::capacity() == 8U,
                "SharedMemorySpscQueue capacity should match the template argument");

  lolakit::core::atomic_store_relaxed(counter, std::uint64_t{7});
  LOLAKIT_ASSERT(singleton.value() == 42U);
  LOLAKIT_ASSERT(lolakit::core::atomic_load_relaxed(counter) == 7U);
  LOLAKIT_ASSERT(padded.value() == 123U);
  LOLAKIT_ASSERT(non_copyable.value() == 7);
  LOLAKIT_ASSERT(queue.empty());
  LOLAKIT_ASSERT(trivial_queue.empty());
#if defined(_WIN32)
  LOLAKIT_ASSERT(shm_producer.is_open());
  LOLAKIT_ASSERT(shm_consumer.is_open());
#else
  LOLAKIT_ASSERT(!shm_producer.is_open());
  LOLAKIT_ASSERT(!shm_consumer.is_open());
#endif

  LOLAKIT_ASSERT(lolakit::core::TscClock::now_cycles() > 0U);
  LOLAKIT_ASSERT(lolakit::core::TscClock::now_ns() > 0U);
  LOLAKIT_ASSERT(lolakit::core::TscClock::cycles_per_ns() > 0.0);

  LOLAKIT_ASSERT(queue.try_push("alpha"));
  LOLAKIT_ASSERT(queue.emplace("beta"));
  LOLAKIT_ASSERT(queue.size() == 2U);

  auto first = queue.try_pop();
  LOLAKIT_ASSERT(first.has_value());
  LOLAKIT_ASSERT(*first == "alpha");

  std::string second;
  LOLAKIT_ASSERT(queue.try_pop(second));
  LOLAKIT_ASSERT(second == "beta");
  LOLAKIT_ASSERT(queue.empty());

  LOLAKIT_ASSERT(trivial_queue.try_push(7U));
  std::uint64_t trivial_value = 0U;
  LOLAKIT_ASSERT(trivial_queue.try_pop(trivial_value));
  LOLAKIT_ASSERT(trivial_value == 7U);
  LOLAKIT_ASSERT(trivial_queue.empty());

#if defined(_WIN32)
  const std::string_view payload = "gamma";
  LOLAKIT_ASSERT(shm_producer.try_push(payload.data(),
                                       static_cast<std::uint32_t>(payload.size())));
  char buffer[64] = {};
  std::uint32_t size = 0U;
  LOLAKIT_ASSERT(shm_consumer.try_pop(buffer, sizeof(buffer), size));
  LOLAKIT_ASSERT(size == payload.size());
  LOLAKIT_ASSERT(std::string(buffer, buffer + size) == payload);
#else
  (void)mapping_name;
#endif

  spin_wait.pause();
  spin_wait.reset();

  return 0;
}
