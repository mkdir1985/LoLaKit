#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

class TestFailure : public std::runtime_error {
 public:
  explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define LOLAKIT_TEST_ASSERT(expr)                                              \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::ostringstream lolakit_test_assert_stream;                           \
      lolakit_test_assert_stream << "Assertion failed: " << #expr              \
                                 << " (" << __FILE__ << ":" << __LINE__       \
                                 << ")";                                       \
      throw ::TestFailure(lolakit_test_assert_stream.str());                   \
    }                                                                          \
  } while (false)

struct TrackedValue {
  static std::atomic<int> live_count;

  int value{0};

  explicit TrackedValue(int input = 0) : value(input) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }

  TrackedValue(const TrackedValue& other) : value(other.value) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }

  TrackedValue(TrackedValue&& other) noexcept : value(other.value) {
    other.value = -1;
    live_count.fetch_add(1, std::memory_order_relaxed);
  }

  TrackedValue& operator=(const TrackedValue& other) {
    value = other.value;
    return *this;
  }

  TrackedValue& operator=(TrackedValue&& other) noexcept {
    value = other.value;
    other.value = -1;
    return *this;
  }

  ~TrackedValue() { live_count.fetch_sub(1, std::memory_order_relaxed); }
};

std::atomic<int> TrackedValue::live_count{0};

struct MagicSingletonProbe
    : public lolakit::core::Singleton<MagicSingletonProbe> {
  friend class lolakit::core::Singleton<MagicSingletonProbe>;

  static std::atomic<int> ctor_count;
  int value{7};

 private:
  MagicSingletonProbe() {
    ctor_count.fetch_add(1, std::memory_order_relaxed);
  }
};

std::atomic<int> MagicSingletonProbe::ctor_count{0};

struct DclpSingletonProbe
    : public lolakit::core::DclpSingleton<DclpSingletonProbe> {
  friend class lolakit::core::DclpSingleton<DclpSingletonProbe>;

  static std::atomic<int> ctor_count;
  int value{11};

 private:
  DclpSingletonProbe() {
    ctor_count.fetch_add(1, std::memory_order_relaxed);
  }
};

std::atomic<int> DclpSingletonProbe::ctor_count{0};

std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

std::string unique_shared_memory_name(const char* suffix) {
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return std::string("unit.") + suffix + "." +
         std::to_string(current_process_id()) + "." + std::to_string(ticks);
}

void test_atomic_helpers() {
  lolakit::core::Atomic<std::uint64_t> value(1U);

  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_relaxed(value) == 1U);
  lolakit::core::atomic_store_release(value, std::uint64_t{7});
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_acquire(value) == 7U);

  const std::uint64_t previous =
      lolakit::core::atomic_exchange_acq_rel(value, std::uint64_t{9});
  LOLAKIT_TEST_ASSERT(previous == 7U);
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_relaxed(value) == 9U);

  std::uint64_t expected = 9U;
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_compare_exchange_strong_acq_rel(
      value, expected, std::uint64_t{11}));
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_relaxed(value) == 11U);

  expected = 10U;
  LOLAKIT_TEST_ASSERT(!lolakit::core::atomic_compare_exchange_strong_acq_rel(
      value, expected, std::uint64_t{13}));
  LOLAKIT_TEST_ASSERT(expected == 11U);

  expected = 11U;
  while (!lolakit::core::atomic_compare_exchange_weak_acq_rel(
      value, expected, std::uint64_t{15})) {
    expected = 11U;
  }
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_relaxed(value) == 15U);

  LOLAKIT_TEST_ASSERT(
      lolakit::core::atomic_fetch_add_relaxed(value, std::uint64_t{5}) == 15U);
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_relaxed(value) == 20U);
  LOLAKIT_TEST_ASSERT(
      lolakit::core::atomic_fetch_sub_relaxed(value, std::uint64_t{3}) == 20U);
  LOLAKIT_TEST_ASSERT(lolakit::core::atomic_load_relaxed(value) == 17U);

  lolakit::core::atomic_thread_fence_acquire();
  lolakit::core::atomic_thread_fence_release();
  lolakit::core::atomic_thread_fence_seq_cst();
}

void test_tsc_clock() {
  lolakit::core::TscClock::initialize(std::chrono::milliseconds(1));
  LOLAKIT_TEST_ASSERT(lolakit::core::TscClock::is_initialized());
  LOLAKIT_TEST_ASSERT(lolakit::core::TscClock::cycles_per_ns() > 0.0);
  LOLAKIT_TEST_ASSERT(lolakit::core::TscClock::ns_per_cycle() > 0.0);

  const std::uint64_t first_cycles = lolakit::core::TscClock::now_cycles();
  const std::uint64_t first_ns = lolakit::core::TscClock::now_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const std::uint64_t second_cycles = lolakit::core::TscClock::now_cycles();
  const std::uint64_t second_ns = lolakit::core::TscClock::now_ns();

  LOLAKIT_TEST_ASSERT(second_cycles >= first_cycles);
  LOLAKIT_TEST_ASSERT(second_ns >= first_ns);
}

template <typename SingletonType>
void run_singleton_concurrency_test(int expected_value) {
  constexpr int kThreadCount = 8;
  std::vector<const SingletonType*> instances(kThreadCount, nullptr);
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&, i]() {
      instances[i] = &SingletonType::instance();
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  LOLAKIT_TEST_ASSERT(instances.front() != nullptr);
  for (const SingletonType* instance : instances) {
    LOLAKIT_TEST_ASSERT(instance == instances.front());
    LOLAKIT_TEST_ASSERT(instance->value == expected_value);
  }
}

void test_magic_singleton() {
  run_singleton_concurrency_test<MagicSingletonProbe>(7);
  LOLAKIT_TEST_ASSERT(MagicSingletonProbe::ctor_count.load(
                          std::memory_order_relaxed) == 1);
}

void test_dclp_singleton() {
  run_singleton_concurrency_test<DclpSingletonProbe>(11);
  LOLAKIT_TEST_ASSERT(DclpSingletonProbe::ctor_count.load(
                          std::memory_order_relaxed) == 1);
}

void test_spsc_capacity_and_fifo() {
  lolakit::core::SpscRingBuffer<int, 8> queue;

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.full());

  for (int i = 0; i < 8; ++i) {
    LOLAKIT_TEST_ASSERT(queue.try_push(i));
  }

  LOLAKIT_TEST_ASSERT(queue.full());
  LOLAKIT_TEST_ASSERT(queue.size() == 8U);
  LOLAKIT_TEST_ASSERT(!queue.try_push(9));

  for (int i = 0; i < 8; ++i) {
    auto value = queue.try_pop();
    LOLAKIT_TEST_ASSERT(value.has_value());
    LOLAKIT_TEST_ASSERT(*value == i);
  }

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.try_pop().has_value());
}

void test_spsc_wraparound_and_clear() {
  LOLAKIT_TEST_ASSERT(TrackedValue::live_count.load(std::memory_order_relaxed) ==
                      0);

  {
    lolakit::core::SpscRingBuffer<TrackedValue, 4> queue;

    for (int i = 0; i < 4; ++i) {
      LOLAKIT_TEST_ASSERT(queue.emplace(i));
    }

    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 4);

    for (int i = 0; i < 2; ++i) {
      auto value = queue.try_pop();
      LOLAKIT_TEST_ASSERT(value.has_value());
      LOLAKIT_TEST_ASSERT(value->value == i);
    }

    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 2);

    LOLAKIT_TEST_ASSERT(queue.emplace(4));
    LOLAKIT_TEST_ASSERT(queue.emplace(5));
    LOLAKIT_TEST_ASSERT(queue.full());

    queue.clear();
    LOLAKIT_TEST_ASSERT(queue.empty());
    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 0);
  }

  LOLAKIT_TEST_ASSERT(TrackedValue::live_count.load(std::memory_order_relaxed) ==
                      0);
}

void test_spsc_threaded_roundtrip() {
  constexpr std::uint64_t kMessageCount = 200000U;
  lolakit::core::SpscRingBuffer<std::uint64_t, 1024> queue;
  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> consumed_sum{0U};

  std::thread producer([&]() {
    lolakit::core::SpinWait wait;
    for (std::uint64_t i = 0; i < kMessageCount; ++i) {
      while (!queue.try_push(i)) {
        wait.pause();
      }
      wait.reset();
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    lolakit::core::SpinWait wait;
    std::uint64_t expected = 0U;
    std::uint64_t local_sum = 0U;

    while (expected < kMessageCount) {
      auto value = queue.try_pop();
      if (!value.has_value()) {
        if (producer_done.load(std::memory_order_acquire)) {
          wait.pause();
        } else {
          wait.pause();
        }
        continue;
      }

      LOLAKIT_TEST_ASSERT(*value == expected);
      local_sum += *value;
      ++expected;
      wait.reset();
    }

    consumed_sum.store(local_sum, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  const std::uint64_t expected_sum =
      (kMessageCount - 1U) * kMessageCount / 2U;
  LOLAKIT_TEST_ASSERT(consumed_sum.load(std::memory_order_acquire) ==
                      expected_sum);
  LOLAKIT_TEST_ASSERT(queue.empty());
}

void test_trivial_spsc_capacity_and_fifo() {
  lolakit::core::TrivialSpscRingBuffer<std::uint64_t, 8> queue;

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.full());

  for (std::uint64_t i = 0; i < 8U; ++i) {
    LOLAKIT_TEST_ASSERT(queue.try_push(i));
  }

  LOLAKIT_TEST_ASSERT(queue.full());
  LOLAKIT_TEST_ASSERT(queue.size() == 8U);

  for (std::uint64_t i = 0; i < 8U; ++i) {
    std::uint64_t value = 0U;
    LOLAKIT_TEST_ASSERT(queue.try_pop(value));
    LOLAKIT_TEST_ASSERT(value == i);
  }

  LOLAKIT_TEST_ASSERT(queue.empty());
}

void test_shared_memory_spsc_queue() {
  using Queue = lolakit::core::SharedMemorySpscQueue<64, 4>;

  const std::string name = unique_shared_memory_name("shared_queue");
  auto producer = Queue::open(name);
  auto consumer = Queue::attach(name);

  LOLAKIT_TEST_ASSERT(producer.is_open());
  LOLAKIT_TEST_ASSERT(consumer.is_open());
  LOLAKIT_TEST_ASSERT(producer.was_created());
  LOLAKIT_TEST_ASSERT(!consumer.was_created());

  struct Message {
    std::uint64_t seq;
    std::uint32_t qty;
  };

  for (std::uint64_t i = 0; i < Queue::capacity(); ++i) {
    Message message{i, static_cast<std::uint32_t>(100U + i)};
    LOLAKIT_TEST_ASSERT(producer.try_push(message));
  }

  LOLAKIT_TEST_ASSERT(producer.full());
  LOLAKIT_TEST_ASSERT(!producer.try_push(Message{99U, 999U}));

  for (std::uint64_t i = 0; i < Queue::capacity(); ++i) {
    Message message{};
    LOLAKIT_TEST_ASSERT(consumer.try_pop(message));
    LOLAKIT_TEST_ASSERT(message.seq == i);
    LOLAKIT_TEST_ASSERT(message.qty == static_cast<std::uint32_t>(100U + i));
  }

  LOLAKIT_TEST_ASSERT(consumer.empty());

  const char payload[] = "shared-message";
  LOLAKIT_TEST_ASSERT(
      producer.try_push(payload, static_cast<std::uint32_t>(sizeof(payload))));

  char buffer[64] = {};
  std::uint32_t size = 0U;
  LOLAKIT_TEST_ASSERT(consumer.try_pop(buffer, sizeof(buffer), size));
  LOLAKIT_TEST_ASSERT(size == sizeof(payload));
  LOLAKIT_TEST_ASSERT(std::memcmp(buffer, payload, sizeof(payload)) == 0);
}

void test_mutex_vector_spsc_capacity_and_fifo() {
  lolakit::core::MutexVectorSpscQueue<int, 8> queue;

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.full());

  for (int i = 0; i < 8; ++i) {
    LOLAKIT_TEST_ASSERT(queue.try_push(i));
  }

  LOLAKIT_TEST_ASSERT(queue.full());
  LOLAKIT_TEST_ASSERT(queue.size() == 8U);
  LOLAKIT_TEST_ASSERT(!queue.try_push(9));

  for (int i = 0; i < 8; ++i) {
    auto value = queue.try_pop();
    LOLAKIT_TEST_ASSERT(value.has_value());
    LOLAKIT_TEST_ASSERT(*value == i);
  }

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.try_pop().has_value());
}

void test_mutex_vector_spsc_wraparound_and_clear() {
  LOLAKIT_TEST_ASSERT(TrackedValue::live_count.load(std::memory_order_relaxed) ==
                      0);

  {
    lolakit::core::MutexVectorSpscQueue<TrackedValue, 4> queue;

    for (int i = 0; i < 4; ++i) {
      LOLAKIT_TEST_ASSERT(queue.emplace(i));
    }

    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 4);

    for (int i = 0; i < 2; ++i) {
      auto value = queue.try_pop();
      LOLAKIT_TEST_ASSERT(value.has_value());
      LOLAKIT_TEST_ASSERT(value->value == i);
    }

    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 2);

    LOLAKIT_TEST_ASSERT(queue.emplace(4));
    LOLAKIT_TEST_ASSERT(queue.emplace(5));
    LOLAKIT_TEST_ASSERT(queue.full());

    queue.clear();
    LOLAKIT_TEST_ASSERT(queue.empty());
    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 0);
  }

  LOLAKIT_TEST_ASSERT(TrackedValue::live_count.load(std::memory_order_relaxed) ==
                      0);
}

void test_mutex_vector_spsc_threaded_roundtrip() {
  constexpr std::uint64_t kMessageCount = 200000U;
  lolakit::core::MutexVectorSpscQueue<std::uint64_t, 1024> queue;
  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> consumed_sum{0U};

  std::thread producer([&]() {
    lolakit::core::SpinWait wait;
    for (std::uint64_t i = 0; i < kMessageCount; ++i) {
      while (!queue.try_push(i)) {
        wait.pause();
      }
      wait.reset();
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    lolakit::core::SpinWait wait;
    std::uint64_t expected = 0U;
    std::uint64_t local_sum = 0U;

    while (expected < kMessageCount) {
      auto value = queue.try_pop();
      if (!value.has_value()) {
        if (producer_done.load(std::memory_order_acquire)) {
          wait.pause();
        } else {
          wait.pause();
        }
        continue;
      }

      LOLAKIT_TEST_ASSERT(*value == expected);
      local_sum += *value;
      ++expected;
      wait.reset();
    }

    consumed_sum.store(local_sum, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  const std::uint64_t expected_sum =
      (kMessageCount - 1U) * kMessageCount / 2U;
  LOLAKIT_TEST_ASSERT(consumed_sum.load(std::memory_order_acquire) ==
                      expected_sum);
  LOLAKIT_TEST_ASSERT(queue.empty());
}

void test_trivial_mutex_vector_spsc_capacity_and_fifo() {
  lolakit::core::TrivialMutexVectorSpscQueue<std::uint64_t, 8> queue;

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.full());

  for (std::uint64_t i = 0; i < 8U; ++i) {
    LOLAKIT_TEST_ASSERT(queue.try_push(i));
  }

  LOLAKIT_TEST_ASSERT(queue.full());
  LOLAKIT_TEST_ASSERT(queue.size() == 8U);

  for (std::uint64_t i = 0; i < 8U; ++i) {
    std::uint64_t value = 0U;
    LOLAKIT_TEST_ASSERT(queue.try_pop(value));
    LOLAKIT_TEST_ASSERT(value == i);
  }

  LOLAKIT_TEST_ASSERT(queue.empty());
}

void test_vector_spsc_capacity_and_fifo() {
  lolakit::core::VectorSpscRingBuffer<int, 8> queue;

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.full());

  for (int i = 0; i < 8; ++i) {
    LOLAKIT_TEST_ASSERT(queue.try_push(i));
  }

  LOLAKIT_TEST_ASSERT(queue.full());
  LOLAKIT_TEST_ASSERT(queue.size() == 8U);
  LOLAKIT_TEST_ASSERT(!queue.try_push(9));

  for (int i = 0; i < 8; ++i) {
    auto value = queue.try_pop();
    LOLAKIT_TEST_ASSERT(value.has_value());
    LOLAKIT_TEST_ASSERT(*value == i);
  }

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.try_pop().has_value());
}

void test_vector_spsc_wraparound_and_clear() {
  LOLAKIT_TEST_ASSERT(TrackedValue::live_count.load(std::memory_order_relaxed) ==
                      0);

  {
    lolakit::core::VectorSpscRingBuffer<TrackedValue, 4> queue;

    for (int i = 0; i < 4; ++i) {
      LOLAKIT_TEST_ASSERT(queue.emplace(i));
    }

    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 4);

    for (int i = 0; i < 2; ++i) {
      auto value = queue.try_pop();
      LOLAKIT_TEST_ASSERT(value.has_value());
      LOLAKIT_TEST_ASSERT(value->value == i);
    }

    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 2);

    LOLAKIT_TEST_ASSERT(queue.emplace(4));
    LOLAKIT_TEST_ASSERT(queue.emplace(5));
    LOLAKIT_TEST_ASSERT(queue.full());

    queue.clear();
    LOLAKIT_TEST_ASSERT(queue.empty());
    LOLAKIT_TEST_ASSERT(
        TrackedValue::live_count.load(std::memory_order_relaxed) == 0);
  }

  LOLAKIT_TEST_ASSERT(TrackedValue::live_count.load(std::memory_order_relaxed) ==
                      0);
}

void test_vector_spsc_threaded_roundtrip() {
  constexpr std::uint64_t kMessageCount = 200000U;
  lolakit::core::VectorSpscRingBuffer<std::uint64_t, 1024> queue;
  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> consumed_sum{0U};

  std::thread producer([&]() {
    lolakit::core::SpinWait wait;
    for (std::uint64_t i = 0; i < kMessageCount; ++i) {
      while (!queue.try_push(i)) {
        wait.pause();
      }
      wait.reset();
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    lolakit::core::SpinWait wait;
    std::uint64_t expected = 0U;
    std::uint64_t local_sum = 0U;

    while (expected < kMessageCount) {
      auto value = queue.try_pop();
      if (!value.has_value()) {
        if (producer_done.load(std::memory_order_acquire)) {
          wait.pause();
        } else {
          wait.pause();
        }
        continue;
      }

      LOLAKIT_TEST_ASSERT(*value == expected);
      local_sum += *value;
      ++expected;
      wait.reset();
    }

    consumed_sum.store(local_sum, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  const std::uint64_t expected_sum =
      (kMessageCount - 1U) * kMessageCount / 2U;
  LOLAKIT_TEST_ASSERT(consumed_sum.load(std::memory_order_acquire) ==
                      expected_sum);
  LOLAKIT_TEST_ASSERT(queue.empty());
}

void test_trivial_vector_spsc_capacity_and_fifo() {
  lolakit::core::TrivialVectorSpscRingBuffer<std::uint64_t, 8> queue;

  LOLAKIT_TEST_ASSERT(queue.empty());
  LOLAKIT_TEST_ASSERT(!queue.full());

  for (std::uint64_t i = 0; i < 8U; ++i) {
    LOLAKIT_TEST_ASSERT(queue.try_push(i));
  }

  LOLAKIT_TEST_ASSERT(queue.full());
  LOLAKIT_TEST_ASSERT(queue.size() == 8U);

  for (std::uint64_t i = 0; i < 8U; ++i) {
    std::uint64_t value = 0U;
    LOLAKIT_TEST_ASSERT(queue.try_pop(value));
    LOLAKIT_TEST_ASSERT(value == i);
  }

  LOLAKIT_TEST_ASSERT(queue.empty());
}

int run_test(const std::string& name, const std::function<void()>& test) {
  try {
    test();
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << name << ": " << ex.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "[FAIL] " << name << ": unknown exception\n";
    return 1;
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"atomic_helpers", test_atomic_helpers},
      {"tsc_clock", test_tsc_clock},
      {"magic_singleton", test_magic_singleton},
      {"dclp_singleton", test_dclp_singleton},
      {"spsc_capacity_and_fifo", test_spsc_capacity_and_fifo},
      {"spsc_wraparound_and_clear", test_spsc_wraparound_and_clear},
      {"spsc_threaded_roundtrip", test_spsc_threaded_roundtrip},
      {"trivial_spsc_capacity_and_fifo", test_trivial_spsc_capacity_and_fifo},
#if defined(_WIN32)
      {"shared_memory_spsc_queue", test_shared_memory_spsc_queue},
#endif
      {"mutex_vector_spsc_capacity_and_fifo", test_mutex_vector_spsc_capacity_and_fifo},
      {"mutex_vector_spsc_wraparound_and_clear", test_mutex_vector_spsc_wraparound_and_clear},
      {"mutex_vector_spsc_threaded_roundtrip", test_mutex_vector_spsc_threaded_roundtrip},
      {"trivial_mutex_vector_spsc_capacity_and_fifo", test_trivial_mutex_vector_spsc_capacity_and_fifo},
      {"vector_spsc_capacity_and_fifo", test_vector_spsc_capacity_and_fifo},
      {"vector_spsc_wraparound_and_clear", test_vector_spsc_wraparound_and_clear},
      {"vector_spsc_threaded_roundtrip", test_vector_spsc_threaded_roundtrip},
      {"trivial_vector_spsc_capacity_and_fifo", test_trivial_vector_spsc_capacity_and_fifo},
  };

  int failures = 0;
  for (const auto& test : tests) {
    failures += run_test(test.first, test.second);
  }

  if (failures == 0) {
    std::cout << "[PASS] all core unit tests passed\n";
  }

  return failures == 0 ? 0 : 1;
}
