#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "lolakit/core.hpp"

namespace {

volatile std::uint64_t g_benchmark_sink = 0U;

#if defined(__GNUC__) || defined(__clang__)
template <typename T>
inline void DO_NOT_OPTIMIZE(T const& value) noexcept {
  asm volatile("" : : "r,m"(value) : "memory");
}
#elif defined(_MSC_VER)
template <typename T>
inline void DO_NOT_OPTIMIZE(T const& value) noexcept {
  _ReadWriteBarrier();
  char const* volatile barrier = reinterpret_cast<char const*>(std::addressof(value));
  (void)barrier;
  _ReadWriteBarrier();
}
#else
template <typename T>
inline void DO_NOT_OPTIMIZE(T const& value) noexcept {
  volatile auto const* p = std::addressof(value);
  (void)p;
}
#endif

struct BenchmarkOptions {
  std::uint64_t iterations{1000000U};
  std::uint64_t warmup_iterations{200000U};
  std::uint64_t batch_size{50000U};
  int affinity_cpu{-1};
  std::string csv_path;
};

struct BatchSample {
  std::uint64_t operations{0U};
  double total_ns{0.0};
  double ns_per_op{0.0};
};

struct BenchmarkResult {
  std::string name;
  std::uint64_t iterations{0U};
  std::uint64_t warmup_iterations{0U};
  std::uint64_t batch_size{0U};
  std::size_t sample_count{0U};
  double total_ns{0.0};
  double mean_ns_per_op{0.0};
  double min_ns_per_op{0.0};
  double p50_ns_per_op{0.0};
  double p95_ns_per_op{0.0};
  double p99_ns_per_op{0.0};
  double max_ns_per_op{0.0};
  double mops{0.0};
  std::string affinity;
  std::string notes;
};

class ThreadAffinityScope {
 public:
  explicit ThreadAffinityScope(int cpu_index) : cpu_index_(cpu_index) {
#if defined(_WIN32)
    if (cpu_index_ < 0 || cpu_index_ >= static_cast<int>(sizeof(DWORD_PTR) * 8U)) {
      return;
    }

    const DWORD_PTR mask = static_cast<DWORD_PTR>(1ULL) << cpu_index_;
    previous_mask_ = ::SetThreadAffinityMask(::GetCurrentThread(), mask);
    applied_ = previous_mask_ != 0;
#else
    if (cpu_index_ < 0) {
      return;
    }
    const int cpu_count = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    if (cpu_index_ >= cpu_count) {
      return;
    }

    cpu_set_t previous_cpuset;
    CPU_ZERO(&previous_cpuset);
    if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t),
                               &previous_cpuset) == 0) {
      previous_set_ = previous_cpuset;
      has_previous_ = true;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_index_, &cpuset);
    applied_ = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
                                      &cpuset) == 0;
#endif
  }

  ~ThreadAffinityScope() {
#if defined(_WIN32)
    if (applied_) {
      (void) ::SetThreadAffinityMask(::GetCurrentThread(), previous_mask_);
    }
#else
    if (applied_ && has_previous_) {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &previous_set_);
    }
#endif
  }

  bool applied() const noexcept { return applied_; }

 private:
  int cpu_index_{-1};
  bool applied_{false};

#if defined(_WIN32)
  DWORD_PTR previous_mask_{0};
#else
  bool has_previous_{false};
  cpu_set_t previous_set_{};
#endif
};

std::uint64_t parse_u64(const std::string& input, const char* option_name) {
  try {
    return static_cast<std::uint64_t>(std::stoull(input));
  } catch (...) {
    std::ostringstream stream;
    stream << "invalid value for " << option_name << ": " << input;
    throw std::runtime_error(stream.str());
  }
}

int parse_int(const std::string& input, const char* option_name) {
  try {
    return std::stoi(input);
  } catch (...) {
    std::ostringstream stream;
    stream << "invalid value for " << option_name << ": " << input;
    throw std::runtime_error(stream.str());
  }
}

BenchmarkOptions parse_options(int argc, char** argv) {
  BenchmarkOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--iterations" && i + 1 < argc) {
      options.iterations = parse_u64(argv[++i], "--iterations");
    } else if (arg == "--warmup-iterations" && i + 1 < argc) {
      options.warmup_iterations = parse_u64(argv[++i], "--warmup-iterations");
    } else if (arg == "--batch-size" && i + 1 < argc) {
      options.batch_size = parse_u64(argv[++i], "--batch-size");
    } else if (arg == "--affinity-cpu" && i + 1 < argc) {
      options.affinity_cpu = parse_int(argv[++i], "--affinity-cpu");
    } else if (arg == "--csv" && i + 1 < argc) {
      options.csv_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: lolakit_core_benchmark [--iterations N] "
             "[--warmup-iterations N] [--batch-size N] "
             "[--affinity-cpu N] [--csv path]\n";
      std::exit(0);
    } else if (!arg.empty() && arg[0] != '-') {
      options.iterations = parse_u64(arg, "iterations");
    } else {
      std::ostringstream stream;
      stream << "unknown argument: " << arg;
      throw std::runtime_error(stream.str());
    }
  }

  if (options.iterations == 0U) {
    options.iterations = 1U;
  }
  if (options.batch_size == 0U) {
    options.batch_size = options.iterations;
  }

  return options;
}

std::vector<std::uint64_t> split_iterations(std::uint64_t total,
                                            std::uint64_t batch_size) {
  if (total == 0U) {
    return {};
  }

  std::vector<std::uint64_t> batches;
  std::uint64_t remaining = total;
  while (remaining > 0U) {
    const std::uint64_t current = std::min(remaining, batch_size);
    batches.push_back(current);
    remaining -= current;
  }
  return batches;
}

double percentile(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());
  const double position = (p / 100.0) * static_cast<double>(values.size() - 1U);
  const auto lower_index = static_cast<std::size_t>(position);
  const auto upper_index = std::min(values.size() - 1U, lower_index + 1U);
  const double fraction = position - static_cast<double>(lower_index);
  return values[lower_index] +
         (values[upper_index] - values[lower_index]) * fraction;
}

int normalize_cpu(int cpu_index) {
  if (cpu_index < 0) {
    return -1;
  }

  const auto hardware_threads = static_cast<int>(std::thread::hardware_concurrency());
  if (hardware_threads <= 0) {
    return cpu_index;
  }

  return cpu_index % hardware_threads;
}

std::string describe_affinity(int cpu_index, bool applied) {
  if (cpu_index < 0) {
    return "none";
  }
  if (!applied) {
    return "cpu" + std::to_string(cpu_index) + ":unsupported";
  }
  return "cpu" + std::to_string(cpu_index);
}

BenchmarkResult build_result(const std::string& name,
                             const BenchmarkOptions& options,
                             const std::vector<BatchSample>& samples,
                             const std::string& affinity,
                             const std::string& notes) {
  BenchmarkResult result;
  result.name = name;
  result.iterations = options.iterations;
  result.warmup_iterations = options.warmup_iterations;
  result.batch_size = options.batch_size;
  result.sample_count = samples.size();
  result.affinity = affinity;
  result.notes = notes;

  std::vector<double> ns_per_op_samples;
  ns_per_op_samples.reserve(samples.size());

  std::uint64_t total_ops = 0U;
  double total_ns = 0.0;
  for (const BatchSample& sample : samples) {
    total_ops += sample.operations;
    total_ns += sample.total_ns;
    ns_per_op_samples.push_back(sample.ns_per_op);
  }

  result.total_ns = total_ns;
  result.mean_ns_per_op =
      total_ops == 0U ? 0.0 : total_ns / static_cast<double>(total_ops);
  result.min_ns_per_op = ns_per_op_samples.empty()
                             ? 0.0
                             : *std::min_element(ns_per_op_samples.begin(),
                                                 ns_per_op_samples.end());
  result.max_ns_per_op = ns_per_op_samples.empty()
                             ? 0.0
                             : *std::max_element(ns_per_op_samples.begin(),
                                                 ns_per_op_samples.end());
  result.p50_ns_per_op = percentile(ns_per_op_samples, 50.0);
  result.p95_ns_per_op = percentile(ns_per_op_samples, 95.0);
  result.p99_ns_per_op = percentile(ns_per_op_samples, 99.0);
  result.mops =
      total_ns == 0.0 ? 0.0 : static_cast<double>(total_ops) / total_ns * 1e3;

  return result;
}

template <typename Fn>
BenchmarkResult run_batched_benchmark(const std::string& name,
                                      const BenchmarkOptions& options,
                                      int affinity_cpu, Fn&& fn) {
  ThreadAffinityScope affinity_scope(affinity_cpu);
  const auto warmup_batches =
      split_iterations(options.warmup_iterations, options.batch_size);
  for (const std::uint64_t operations : warmup_batches) {
    g_benchmark_sink ^= fn(operations);
  }

  const auto measured_batches =
      split_iterations(options.iterations, options.batch_size);
  std::vector<BatchSample> samples;
  samples.reserve(measured_batches.size());

  for (const std::uint64_t operations : measured_batches) {
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t batch_sink = fn(operations);
    const auto end = std::chrono::steady_clock::now();

    const double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    samples.push_back(
        BatchSample{operations, total_ns,
                    total_ns / static_cast<double>(operations)});
    g_benchmark_sink ^= batch_sink;
  }

  return build_result(name, options, samples,
                      describe_affinity(affinity_cpu, affinity_scope.applied()),
                      "batched");
}

std::pair<std::vector<std::uint64_t>, std::vector<std::uint64_t>>
prepare_two_thread_batches(const BenchmarkOptions& options) {
  return std::make_pair(
      split_iterations(options.warmup_iterations, options.batch_size),
      split_iterations(options.iterations, options.batch_size));
}

template <typename QueueT, typename ProducerFn, typename ConsumerFn>
BenchmarkResult run_two_thread_benchmark(const std::string& name,
                                         const BenchmarkOptions& options,
                                         const std::string& notes,
                                         ProducerFn&& producer_fn,
                                         ConsumerFn&& consumer_fn) {
  const int producer_cpu = normalize_cpu(options.affinity_cpu);
  const int consumer_cpu =
      options.affinity_cpu < 0 ? -1 : normalize_cpu(options.affinity_cpu + 1);

  const auto batch_pair = prepare_two_thread_batches(options);
  const auto& warmup_batches = batch_pair.first;
  const auto& measured_batches = batch_pair.second;

  std::vector<std::uint64_t> all_batches;
  all_batches.reserve(warmup_batches.size() + measured_batches.size());
  all_batches.insert(all_batches.end(), warmup_batches.begin(), warmup_batches.end());
  all_batches.insert(all_batches.end(), measured_batches.begin(), measured_batches.end());

  std::vector<std::chrono::steady_clock::time_point> batch_start(all_batches.size());
  std::vector<std::chrono::steady_clock::time_point> batch_end(all_batches.size());

  QueueT queue;
  std::uint64_t consumer_sum = 0U;
  bool producer_affinity_applied = false;
  bool consumer_affinity_applied = false;

  std::thread producer([&]() {
    ThreadAffinityScope affinity_scope(producer_cpu);
    producer_affinity_applied = affinity_scope.applied();
    lolakit::core::SpinWait wait;
    std::uint64_t next_value = 0U;

    for (std::size_t batch = 0; batch < all_batches.size(); ++batch) {
      batch_start[batch] = std::chrono::steady_clock::now();
      for (std::uint64_t i = 0; i < all_batches[batch]; ++i) {
        while (!producer_fn(queue, next_value, wait)) {
          wait.pause();
        }
        ++next_value;
        wait.reset();
      }
    }
  });

  std::thread consumer([&]() {
    ThreadAffinityScope affinity_scope(consumer_cpu);
    consumer_affinity_applied = affinity_scope.applied();
    lolakit::core::SpinWait wait;

    for (std::size_t batch = 0; batch < all_batches.size(); ++batch) {
      for (std::uint64_t i = 0; i < all_batches[batch]; ++i) {
        auto value = consumer_fn(queue, wait);
        while (!value.has_value()) {
          wait.pause();
          value = consumer_fn(queue, wait);
        }
        consumer_sum += *value;
        wait.reset();
      }
      batch_end[batch] = std::chrono::steady_clock::now();
    }
  });

  producer.join();
  consumer.join();

  g_benchmark_sink ^= consumer_sum;

  std::vector<BatchSample> samples;
  samples.reserve(measured_batches.size());
  const std::size_t warmup_count = warmup_batches.size();
  for (std::size_t i = 0; i < measured_batches.size(); ++i) {
    const std::size_t batch_index = warmup_count + i;
    const double total_ns = static_cast<double>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(batch_end[batch_index] - batch_start[batch_index])
                                                    .count());
    const std::uint64_t operations = measured_batches[i];
    samples.push_back(
        BatchSample{operations, total_ns,
                    total_ns / static_cast<double>(operations)});
  }

  const std::string affinity =
      describe_affinity(producer_cpu, producer_affinity_applied) + "/" +
      describe_affinity(consumer_cpu, consumer_affinity_applied);
  return build_result(name, options, samples, affinity, notes);
}

template <typename QueueT>
BenchmarkResult run_generic_two_thread_benchmark(const std::string& name,
                                                 const BenchmarkOptions& options,
                                                 const std::string& notes) {
  auto producer_op = [](QueueT& queue, std::uint64_t next_value,
                        lolakit::core::SpinWait& /*wait*/) -> bool {
    return queue.try_push(next_value);
  };
  auto consumer_op = [](QueueT& queue,
                        lolakit::core::SpinWait& /*wait*/) -> std::optional<std::uint64_t> {
    return queue.try_pop();
  };
  return run_two_thread_benchmark<QueueT>(name, options, notes,
                                          std::move(producer_op),
                                          std::move(consumer_op));
}

BenchmarkResult run_spsc_two_thread_benchmark(const BenchmarkOptions& options) {
  return run_generic_two_thread_benchmark<
      lolakit::core::SpscRingBuffer<std::uint64_t, 1024>>(
      "spsc_two_thread", options, "producer_consumer_end_to_end");
}

BenchmarkResult run_mutex_vector_spsc_two_thread_benchmark(
    const BenchmarkOptions& options) {
  return run_generic_two_thread_benchmark<
      lolakit::core::MutexVectorSpscQueue<std::uint64_t, 1024>>(
      "mutex_vector_spsc_two_thread", options,
      "mutex_vector_producer_consumer_end_to_end");
}

BenchmarkResult run_vector_spsc_two_thread_benchmark(
    const BenchmarkOptions& options) {
  return run_generic_two_thread_benchmark<
      lolakit::core::VectorSpscRingBuffer<std::uint64_t, 1024>>(
      "vector_spsc_two_thread", options,
      "vector_spsc_lockfree_producer_consumer_end_to_end");
}

template <typename CounterPair>
BenchmarkResult run_false_sharing_benchmark(const std::string& name,
                                            const BenchmarkOptions& options,
                                            const std::string& notes) {
  const int first_cpu = normalize_cpu(options.affinity_cpu);
  const int second_cpu =
      options.affinity_cpu < 0 ? -1 : normalize_cpu(options.affinity_cpu + 1);

  const auto warmup_batches =
      split_iterations(options.warmup_iterations, options.batch_size);
  const auto measured_batches =
      split_iterations(options.iterations, options.batch_size);

  std::vector<std::uint64_t> all_batches;
  all_batches.reserve(warmup_batches.size() + measured_batches.size());
  all_batches.insert(all_batches.end(), warmup_batches.begin(), warmup_batches.end());
  all_batches.insert(all_batches.end(), measured_batches.begin(), measured_batches.end());

  std::vector<std::chrono::steady_clock::time_point> batch_start(all_batches.size());
  std::vector<std::chrono::steady_clock::time_point> batch_end(all_batches.size());

  CounterPair counters;
  std::atomic<std::size_t> current_batch{
      std::numeric_limits<std::size_t>::max()};
  std::atomic<int> completed_threads{0};
  bool first_affinity_applied = false;
  bool second_affinity_applied = false;

  auto worker = [&](auto& counter, int cpu, bool& affinity_applied) {
    ThreadAffinityScope affinity_scope(cpu);
    affinity_applied = affinity_scope.applied();
    lolakit::core::SpinWait wait;

    for (std::size_t batch = 0; batch < all_batches.size(); ++batch) {
      while (current_batch.load(std::memory_order_acquire) != batch) {
        wait.pause();
      }

      for (std::uint64_t i = 0; i < all_batches[batch]; ++i) {
        (void) lolakit::core::atomic_fetch_add_relaxed(counter, std::uint64_t{1});
      }

      completed_threads.fetch_add(1, std::memory_order_release);
      wait.reset();

      while (current_batch.load(std::memory_order_acquire) == batch) {
        wait.pause();
      }
    }
  };

  std::thread first_worker(
      [&]() { worker(counters.first_ref(), first_cpu, first_affinity_applied); });
  std::thread second_worker([&]() {
    worker(counters.second_ref(), second_cpu, second_affinity_applied);
  });

  for (std::size_t batch = 0; batch < all_batches.size(); ++batch) {
    lolakit::core::atomic_store_relaxed(counters.first_ref(), std::uint64_t{0});
    lolakit::core::atomic_store_relaxed(counters.second_ref(), std::uint64_t{0});
    completed_threads.store(0, std::memory_order_relaxed);

    batch_start[batch] = std::chrono::steady_clock::now();
    current_batch.store(batch, std::memory_order_release);

    while (completed_threads.load(std::memory_order_acquire) != 2) {
      lolakit::core::cpu_relax();
    }

    batch_end[batch] = std::chrono::steady_clock::now();
    g_benchmark_sink ^= lolakit::core::atomic_load_relaxed(counters.first_ref());
    g_benchmark_sink ^= lolakit::core::atomic_load_relaxed(counters.second_ref());
    current_batch.store(batch + 1U, std::memory_order_release);
  }

  first_worker.join();
  second_worker.join();

  std::vector<BatchSample> samples;
  samples.reserve(measured_batches.size());
  const std::size_t warmup_count = warmup_batches.size();
  for (std::size_t i = 0; i < measured_batches.size(); ++i) {
    const std::size_t batch_index = warmup_count + i;
    const double total_ns = static_cast<double>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(batch_end[batch_index] - batch_start[batch_index])
                                                    .count());
    const std::uint64_t operations = measured_batches[i] * 2U;
    samples.push_back(
        BatchSample{operations, total_ns,
                    total_ns / static_cast<double>(operations)});
  }

  const std::string affinity =
      describe_affinity(first_cpu, first_affinity_applied) + "/" +
      describe_affinity(second_cpu, second_affinity_applied);
  return build_result(name, options, samples, affinity, notes);
}

struct UnpaddedCounters {
  lolakit::core::Atomic<std::uint64_t> first{0U};
  lolakit::core::Atomic<std::uint64_t> second{0U};

  lolakit::core::Atomic<std::uint64_t>& first_ref() noexcept { return first; }
  lolakit::core::Atomic<std::uint64_t>& second_ref() noexcept { return second; }
};

struct PaddedCounters {
  lolakit::core::CacheLinePadded<lolakit::core::Atomic<std::uint64_t>> first{0U};
  lolakit::core::CacheLinePadded<lolakit::core::Atomic<std::uint64_t>> second{0U};

  lolakit::core::Atomic<std::uint64_t>& first_ref() noexcept { return first.value(); }
  lolakit::core::Atomic<std::uint64_t>& second_ref() noexcept {
    return second.value();
  }
};

struct BenchmarkMagicSingleton
    : public lolakit::core::Singleton<BenchmarkMagicSingleton> {
  friend class lolakit::core::Singleton<BenchmarkMagicSingleton>;

  std::uint64_t tag() const noexcept { return 0x13579BDF2468ACE0ULL; }

 private:
  BenchmarkMagicSingleton() = default;
};

struct BenchmarkDclpSingleton
    : public lolakit::core::DclpSingleton<BenchmarkDclpSingleton> {
  friend class lolakit::core::DclpSingleton<BenchmarkDclpSingleton>;

  std::uint64_t tag() const noexcept { return 0x02468ACE13579BDFULL; }

 private:
  BenchmarkDclpSingleton() = default;
};

LOLAKIT_NOINLINE std::uint64_t benchmark_magic_singleton_access() {
  const auto& instance = BenchmarkMagicSingleton::instance();
  return instance.tag() ^
         static_cast<std::uint64_t>(
             reinterpret_cast<std::uintptr_t>(&instance));
}

LOLAKIT_NOINLINE std::uint64_t benchmark_dclp_singleton_access() {
  const auto& instance = BenchmarkDclpSingleton::instance();
  return instance.tag() ^
         static_cast<std::uint64_t>(
             reinterpret_cast<std::uintptr_t>(&instance));
}

void print_configuration(const BenchmarkOptions& options) {
  std::cout << "LoLaKit core benchmark\n";
  std::cout << "iterations=" << options.iterations
            << ", warmup_iterations=" << options.warmup_iterations
            << ", batch_size=" << options.batch_size
            << ", affinity_cpu=" << options.affinity_cpu << "\n";
  if (!options.csv_path.empty()) {
    std::cout << "csv=" << options.csv_path << "\n";
  }
}

void print_result(const BenchmarkResult& result) {
  std::cout << std::left << std::setw(24) << result.name << "  "
            << "mean " << std::setw(10) << std::fixed << std::setprecision(2)
            << result.mean_ns_per_op << " ns/op  "
            << "p50 " << std::setw(10) << result.p50_ns_per_op << "  "
            << "p95 " << std::setw(10) << result.p95_ns_per_op << "  "
            << "p99 " << std::setw(10) << result.p99_ns_per_op << "  "
            << "min " << std::setw(10) << result.min_ns_per_op << "  "
            << "max " << std::setw(10) << result.max_ns_per_op << "  "
            << std::setw(10) << result.mops << " Mops/s  "
            << result.affinity << "\n";
}

std::string csv_escape(const std::string& value) {
  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped += ch;
    }
  }
  escaped += "\"";
  return escaped;
}

void write_csv(const std::string& csv_path,
               const std::vector<BenchmarkResult>& results) {
  if (csv_path.empty()) {
    return;
  }

  const std::filesystem::path path(csv_path);
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }

  std::ofstream output(path, std::ios::trunc);
  output << "name,iterations,warmup_iterations,batch_size,sample_count,total_ns,"
            "mean_ns_per_op,min_ns_per_op,p50_ns_per_op,p95_ns_per_op,"
            "p99_ns_per_op,max_ns_per_op,mops,affinity,notes\n";

  for (const BenchmarkResult& result : results) {
    output << csv_escape(result.name) << ","
           << result.iterations << ","
           << result.warmup_iterations << ","
           << result.batch_size << ","
           << result.sample_count << ","
           << std::fixed << std::setprecision(4)
           << result.total_ns << ","
           << result.mean_ns_per_op << ","
           << result.min_ns_per_op << ","
           << result.p50_ns_per_op << ","
           << result.p95_ns_per_op << ","
           << result.p99_ns_per_op << ","
           << result.max_ns_per_op << ","
           << result.mops << ","
           << csv_escape(result.affinity) << ","
           << csv_escape(result.notes) << "\n";
  }
}

void print_progress_header(std::size_t total) {
  std::cout << "progress: running " << total << " benchmarks; "
            << "each line prints mean/p50/p95/p99 (ns/op)\n"
            << std::flush;
}

void print_progress_start(std::size_t index, std::size_t total,
                          const std::string& name) {
  std::cout << "[" << std::setw(2) << (index + 1) << "/" << total << "] "
            << std::left << std::setw(48) << name << " ... " << std::flush;
}

void print_progress_result(const BenchmarkResult& result) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);
  oss << "mean=" << std::setw(10) << result.mean_ns_per_op
      << "  p50=" << std::setw(10) << result.p50_ns_per_op
      << "  p95=" << std::setw(10) << result.p95_ns_per_op
      << "  p99=" << std::setw(10) << result.p99_ns_per_op;
  if (result.mops > 0.0) {
    oss << "  " << std::setprecision(2)
        << std::setw(8) << result.mops << " Mops/s";
  }
  std::cout << oss.str() << "\n" << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const BenchmarkOptions options = parse_options(argc, argv);
    print_configuration(options);

    lolakit::core::TscClock::initialize(std::chrono::milliseconds(1));

    const std::size_t kBenchmarkCount = 20;
    print_progress_header(kBenchmarkCount);
    std::size_t idx = 0;

    auto run_with_progress =
        [&](const std::string& name,
            const std::function<BenchmarkResult(void)>& run) -> BenchmarkResult {
          print_progress_start(idx++, kBenchmarkCount, name);
          BenchmarkResult result = run();
          print_progress_result(result);
          return result;
        };

    std::vector<BenchmarkResult> results;
    results.reserve(kBenchmarkCount);

    results.push_back(run_with_progress("atomic_fetch_add_relaxed", [&] {
      return run_batched_benchmark(
          "atomic_fetch_add_relaxed", options, normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::Atomic<std::uint64_t> counter(0U);
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              local_sum += lolakit::core::atomic_fetch_add_relaxed(
                  counter, std::uint64_t{1});
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("tsc_now_cycles", [&] {
      return run_batched_benchmark(
          "tsc_now_cycles", options, normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              local_sum += lolakit::core::TscClock::now_cycles();
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("singleton_magic_static", [&] {
      return run_batched_benchmark(
          "singleton_magic_static", options, normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              local_sum ^= benchmark_magic_singleton_access();
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("singleton_dclp", [&] {
      return run_batched_benchmark(
          "singleton_dclp", options, normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              local_sum ^= benchmark_dclp_singleton_access();
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("spsc_single_thread", [&] {
      return run_batched_benchmark(
          "spsc_single_thread", options, normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::SpscRingBuffer<std::uint64_t, 1024> queue;
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              LOLAKIT_ASSERT(queue.try_push(i));
              auto value = queue.try_pop();
              LOLAKIT_ASSERT(value.has_value());
              DO_NOT_OPTIMIZE(*value);
              local_sum += *value;
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("spsc_single_thread_out_param", [&] {
      return run_batched_benchmark(
          "spsc_single_thread_out_param", options,
          normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::SpscRingBuffer<std::uint64_t, 1024> queue;
            std::uint64_t local_sum = 0U;
            std::uint64_t value = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              LOLAKIT_ASSERT(queue.try_push(i));
              LOLAKIT_ASSERT(queue.try_pop(value));
              DO_NOT_OPTIMIZE(value);
              local_sum += value;
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("trivial_spsc_single_thread", [&] {
      return run_batched_benchmark(
          "trivial_spsc_single_thread", options,
          normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::TrivialSpscRingBuffer<std::uint64_t, 1024> queue;
            std::uint64_t local_sum = 0U;
            std::uint64_t value = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              LOLAKIT_ASSERT(queue.try_push(i));
              LOLAKIT_ASSERT(queue.try_pop(value));
              DO_NOT_OPTIMIZE(value);
              local_sum += value;
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("mutex_vector_spsc_single_thread", [&] {
      return run_batched_benchmark(
          "mutex_vector_spsc_single_thread", options,
          normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::MutexVectorSpscQueue<std::uint64_t, 1024> queue;
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              LOLAKIT_ASSERT(queue.try_push(i));
              auto value = queue.try_pop();
              LOLAKIT_ASSERT(value.has_value());
              DO_NOT_OPTIMIZE(*value);
              local_sum += *value;
            }
            return local_sum;
          });
    }));

    results.push_back(
        run_with_progress("mutex_vector_spsc_single_thread_out_param", [&] {
          return run_batched_benchmark(
              "mutex_vector_spsc_single_thread_out_param", options,
              normalize_cpu(options.affinity_cpu),
              [](std::uint64_t operations) {
                lolakit::core::MutexVectorSpscQueue<std::uint64_t, 1024> queue;
                std::uint64_t local_sum = 0U;
                std::uint64_t value = 0U;
                for (std::uint64_t i = 0; i < operations; ++i) {
                  LOLAKIT_ASSERT(queue.try_push(i));
                  LOLAKIT_ASSERT(queue.try_pop(value));
                  DO_NOT_OPTIMIZE(value);
                  local_sum += value;
                }
                return local_sum;
              });
        }));

    results.push_back(
        run_with_progress("trivial_mutex_vector_spsc_single_thread", [&] {
          return run_batched_benchmark(
              "trivial_mutex_vector_spsc_single_thread", options,
              normalize_cpu(options.affinity_cpu),
              [](std::uint64_t operations) {
                lolakit::core::TrivialMutexVectorSpscQueue<std::uint64_t, 1024> queue;
                std::uint64_t local_sum = 0U;
                std::uint64_t value = 0U;
                for (std::uint64_t i = 0; i < operations; ++i) {
                  LOLAKIT_ASSERT(queue.try_push(i));
                  LOLAKIT_ASSERT(queue.try_pop(value));
                  DO_NOT_OPTIMIZE(value);
                  local_sum += value;
                }
                return local_sum;
              });
        }));

    results.push_back(run_with_progress("vector_spsc_single_thread", [&] {
      return run_batched_benchmark(
          "vector_spsc_single_thread", options,
          normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::VectorSpscRingBuffer<std::uint64_t, 1024> queue;
            std::uint64_t local_sum = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              LOLAKIT_ASSERT(queue.try_push(i));
              auto value = queue.try_pop();
              LOLAKIT_ASSERT(value.has_value());
              DO_NOT_OPTIMIZE(*value);
              local_sum += *value;
            }
            return local_sum;
          });
    }));

    results.push_back(
        run_with_progress("vector_spsc_single_thread_out_param", [&] {
          return run_batched_benchmark(
              "vector_spsc_single_thread_out_param", options,
              normalize_cpu(options.affinity_cpu),
              [](std::uint64_t operations) {
                lolakit::core::VectorSpscRingBuffer<std::uint64_t, 1024> queue;
                std::uint64_t local_sum = 0U;
                std::uint64_t value = 0U;
                for (std::uint64_t i = 0; i < operations; ++i) {
                  LOLAKIT_ASSERT(queue.try_push(i));
                  LOLAKIT_ASSERT(queue.try_pop(value));
                  DO_NOT_OPTIMIZE(value);
                  local_sum += value;
                }
                return local_sum;
              });
        }));

    results.push_back(run_with_progress("trivial_vector_spsc_single_thread", [&] {
      return run_batched_benchmark(
          "trivial_vector_spsc_single_thread", options,
          normalize_cpu(options.affinity_cpu),
          [](std::uint64_t operations) {
            lolakit::core::TrivialVectorSpscRingBuffer<std::uint64_t, 1024> queue;
            std::uint64_t local_sum = 0U;
            std::uint64_t value = 0U;
            for (std::uint64_t i = 0; i < operations; ++i) {
              LOLAKIT_ASSERT(queue.try_push(i));
              LOLAKIT_ASSERT(queue.try_pop(value));
              DO_NOT_OPTIMIZE(value);
              local_sum += value;
            }
            return local_sum;
          });
    }));

    results.push_back(run_with_progress("false_sharing_unpadded", [&] {
      return run_false_sharing_benchmark<UnpaddedCounters>(
          "false_sharing_unpadded", options, "adjacent_atomic_counters");
    }));

    results.push_back(run_with_progress("false_sharing_padded", [&] {
      return run_false_sharing_benchmark<PaddedCounters>(
          "false_sharing_padded", options, "cacheline_padded_atomic_counters");
    }));

    results.push_back(run_with_progress("spsc_two_thread", [&] {
      return run_spsc_two_thread_benchmark(options);
    }));

    results.push_back(run_with_progress("mutex_vector_spsc_two_thread", [&] {
      return run_mutex_vector_spsc_two_thread_benchmark(options);
    }));

    results.push_back(run_with_progress("vector_spsc_two_thread", [&] {
      return run_vector_spsc_two_thread_benchmark(options);
    }));

    std::cout << "\nsink=" << g_benchmark_sink << "\n";
    for (const BenchmarkResult& result : results) {
      print_result(result);
    }

    write_csv(options.csv_path, results);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "benchmark failed: " << ex.what() << "\n";
    return 1;
  }
}
