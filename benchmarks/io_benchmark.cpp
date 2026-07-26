#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
#error "io_benchmark currently supports Windows only."
#endif

#include "lolakit/core.hpp"

namespace {

volatile std::uint64_t g_io_benchmark_sink = 0U;

struct BenchmarkOptions {
  std::uint64_t iterations{200000U};
  std::uint64_t warmup_iterations{50000U};
  std::uint64_t batch_size{10000U};
  std::size_t block_size{256U};
  int affinity_cpu{-1};
  std::string csv_path;
  std::string temp_directory;
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
  std::size_t block_size{0U};
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
    if (cpu_index_ < 0 || cpu_index_ >= static_cast<int>(sizeof(DWORD_PTR) * 8U)) {
      return;
    }

    const DWORD_PTR mask = static_cast<DWORD_PTR>(1ULL) << cpu_index_;
    previous_mask_ = ::SetThreadAffinityMask(::GetCurrentThread(), mask);
    applied_ = previous_mask_ != 0;
  }

  ~ThreadAffinityScope() {
    if (applied_) {
      (void) ::SetThreadAffinityMask(::GetCurrentThread(), previous_mask_);
    }
  }

  bool applied() const noexcept { return applied_; }

 private:
  int cpu_index_{-1};
  bool applied_{false};
  DWORD_PTR previous_mask_{0};
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

std::size_t parse_size(const std::string& input, const char* option_name) {
  return static_cast<std::size_t>(parse_u64(input, option_name));
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
    } else if (arg == "--block-size" && i + 1 < argc) {
      options.block_size = parse_size(argv[++i], "--block-size");
    } else if (arg == "--affinity-cpu" && i + 1 < argc) {
      options.affinity_cpu = parse_int(argv[++i], "--affinity-cpu");
    } else if (arg == "--csv" && i + 1 < argc) {
      options.csv_path = argv[++i];
    } else if (arg == "--temp-directory" && i + 1 < argc) {
      options.temp_directory = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: lolakit_io_benchmark [--iterations N] "
             "[--warmup-iterations N] [--batch-size N] [--block-size bytes] "
             "[--affinity-cpu N] [--csv path] [--temp-directory path]\n";
      std::exit(0);
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
  if (options.block_size == 0U) {
    options.block_size = 1U;
  }

  return options;
}

std::vector<std::uint64_t> split_iterations(std::uint64_t total,
                                            std::uint64_t batch_size) {
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

std::wstring widen(const std::string& value) {
  return std::wstring(value.begin(), value.end());
}

std::filesystem::path make_temp_path(const BenchmarkOptions& options,
                                     const std::string& benchmark_name) {
  std::filesystem::path directory = options.temp_directory.empty()
                                        ? std::filesystem::temp_directory_path()
                                        : std::filesystem::path(options.temp_directory);
  std::filesystem::create_directories(directory);
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return directory / (benchmark_name + "_" + std::to_string(::GetCurrentProcessId()) +
                      "_" + std::to_string(stamp) + ".bin");
}

void preallocate_file(HANDLE file, std::uint64_t size) {
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(size);
  if (!::SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
    throw std::runtime_error("SetFilePointerEx failed during file preallocation");
  }
  if (!::SetEndOfFile(file)) {
    throw std::runtime_error("SetEndOfFile failed during file preallocation");
  }
  position.QuadPart = 0;
  if (!::SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
    throw std::runtime_error("SetFilePointerEx failed while rewinding file");
  }
}

class DirectFileWriter {
 public:
  DirectFileWriter(const BenchmarkOptions& options, const std::string& benchmark_name)
      : path_(make_temp_path(options, benchmark_name)),
        buffer_(options.block_size, 0x5AU) {
    const std::uint64_t total_operations =
        options.iterations + options.warmup_iterations;
    const std::uint64_t total_bytes =
        std::max<std::uint64_t>(1U, total_operations * options.block_size);

    file_ = ::CreateFileW(widen(path_.string()).c_str(), GENERIC_READ | GENERIC_WRITE,
                          0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("CreateFileW failed for direct writer");
    }

    preallocate_file(file_, total_bytes);
  }

  ~DirectFileWriter() {
    if (file_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(file_);
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  std::uint64_t write_batch(std::uint64_t operations) {
    return write_batch_impl(operations, false);
  }

  std::uint64_t write_batch_flush(std::uint64_t operations) {
    return write_batch_impl(operations, true);
  }

 private:
  std::uint64_t write_batch_impl(std::uint64_t operations, bool flush_each_write) {
    std::uint64_t local_sum = 0U;
    for (std::uint64_t i = 0; i < operations; ++i) {
      DWORD written = 0U;
      if (!::WriteFile(file_, buffer_.data(), static_cast<DWORD>(buffer_.size()),
                       &written, nullptr) ||
          written != buffer_.size()) {
        throw std::runtime_error("WriteFile failed during direct benchmark");
      }
      if (flush_each_write && !::FlushFileBuffers(file_)) {
        throw std::runtime_error("FlushFileBuffers failed during direct benchmark");
      }
      local_sum ^= static_cast<std::uint64_t>(buffer_[i % buffer_.size()]);
    }
    return local_sum;
  }

  std::filesystem::path path_;
  HANDLE file_{INVALID_HANDLE_VALUE};
  std::vector<std::uint8_t> buffer_;
};

class MappedFileWriter {
 public:
  MappedFileWriter(const BenchmarkOptions& options, const std::string& benchmark_name)
      : path_(make_temp_path(options, benchmark_name)),
        buffer_(options.block_size, 0xA5U),
        block_size_(options.block_size) {
    const std::uint64_t total_operations =
        options.iterations + options.warmup_iterations;
    total_bytes_ = std::max<std::uint64_t>(1U, total_operations * block_size_);

    file_ = ::CreateFileW(widen(path_.string()).c_str(), GENERIC_READ | GENERIC_WRITE,
                          0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("CreateFileW failed for mapped writer");
    }

    preallocate_file(file_, total_bytes_);

    const DWORD size_high = static_cast<DWORD>(total_bytes_ >> 32U);
    const DWORD size_low = static_cast<DWORD>(total_bytes_ & 0xFFFFFFFFULL);
    mapping_ = ::CreateFileMappingW(file_, nullptr, PAGE_READWRITE, size_high, size_low,
                                    nullptr);
    if (mapping_ == nullptr) {
      throw std::runtime_error("CreateFileMappingW failed for mapped writer");
    }

    view_ = static_cast<std::uint8_t*>(
        ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0U, 0U,
                        static_cast<SIZE_T>(total_bytes_)));
    if (view_ == nullptr) {
      throw std::runtime_error("MapViewOfFile failed for mapped writer");
    }
  }

  ~MappedFileWriter() {
    if (view_ != nullptr) {
      ::UnmapViewOfFile(view_);
    }
    if (mapping_ != nullptr) {
      ::CloseHandle(mapping_);
    }
    if (file_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(file_);
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  std::uint64_t write_batch(std::uint64_t operations) {
    return write_batch_impl(operations, false);
  }

  std::uint64_t write_batch_flush(std::uint64_t operations) {
    return write_batch_impl(operations, true);
  }

 private:
  std::uint64_t write_batch_impl(std::uint64_t operations, bool flush_each_write) {
    std::uint64_t local_sum = 0U;
    for (std::uint64_t i = 0; i < operations; ++i) {
      std::uint8_t* destination = view_ + offset_bytes_;
      std::memcpy(destination, buffer_.data(), block_size_);
      if (flush_each_write) {
        if (!::FlushViewOfFile(destination, block_size_)) {
          throw std::runtime_error("FlushViewOfFile failed during mapped benchmark");
        }
        if (!::FlushFileBuffers(file_)) {
          throw std::runtime_error("FlushFileBuffers failed during mapped benchmark");
        }
      }
      local_sum ^= static_cast<std::uint64_t>(view_[offset_bytes_]);
      offset_bytes_ += block_size_;
    }
    return local_sum;
  }

  std::filesystem::path path_;
  HANDLE file_{INVALID_HANDLE_VALUE};
  HANDLE mapping_{nullptr};
  std::uint8_t* view_{nullptr};
  std::vector<std::uint8_t> buffer_;
  std::uint64_t total_bytes_{0U};
  std::size_t block_size_{0U};
  std::uint64_t offset_bytes_{0U};
};

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
  result.block_size = options.block_size;
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
                                      int affinity_cpu, const std::string& notes,
                                      Fn&& fn) {
  ThreadAffinityScope affinity_scope(affinity_cpu);
  const auto warmup_batches =
      split_iterations(options.warmup_iterations, options.batch_size);
  for (const std::uint64_t operations : warmup_batches) {
    g_io_benchmark_sink ^= fn(operations);
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
    g_io_benchmark_sink ^= batch_sink;
  }

  return build_result(name, options, samples,
                      describe_affinity(affinity_cpu, affinity_scope.applied()),
                      notes);
}

void print_configuration(const BenchmarkOptions& options) {
  std::cout << "LoLaKit io benchmark\n";
  std::cout << "iterations=" << options.iterations
            << ", warmup_iterations=" << options.warmup_iterations
            << ", batch_size=" << options.batch_size
            << ", block_size=" << options.block_size
            << ", affinity_cpu=" << options.affinity_cpu << "\n";
  if (!options.temp_directory.empty()) {
    std::cout << "temp_directory=" << options.temp_directory << "\n";
  }
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
  output << "name,iterations,warmup_iterations,batch_size,block_size,sample_count,"
            "total_ns,mean_ns_per_op,min_ns_per_op,p50_ns_per_op,p95_ns_per_op,"
            "p99_ns_per_op,max_ns_per_op,mops,affinity,notes\n";

  for (const BenchmarkResult& result : results) {
    output << csv_escape(result.name) << ","
           << result.iterations << ","
           << result.warmup_iterations << ","
           << result.batch_size << ","
           << result.block_size << ","
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

}  // namespace

int main(int argc, char** argv) {
  try {
    const BenchmarkOptions options = parse_options(argc, argv);
    print_configuration(options);

    const int affinity_cpu = normalize_cpu(options.affinity_cpu);
    std::vector<BenchmarkResult> results;
    {
      DirectFileWriter direct_writer(options, "direct_write");
      results.push_back(run_batched_benchmark(
          "file_write_handle", options, affinity_cpu, "buffered_sequential_write",
          [&](std::uint64_t operations) {
            return direct_writer.write_batch(operations);
          }));
    }

    {
      DirectFileWriter direct_writer_flush(options, "direct_write_flush");
      results.push_back(run_batched_benchmark(
          "file_write_handle_flush", options, affinity_cpu,
          "buffered_write_with_flush_file_buffers",
          [&](std::uint64_t operations) {
            return direct_writer_flush.write_batch_flush(operations);
          }));
    }

    {
      MappedFileWriter mapped_writer(options, "mapped_write");
      results.push_back(run_batched_benchmark(
          "file_write_mmap", options, affinity_cpu, "mapped_memcpy_write",
          [&](std::uint64_t operations) {
            return mapped_writer.write_batch(operations);
          }));
    }

    {
      MappedFileWriter mapped_writer_flush(options, "mapped_write_flush");
      results.push_back(run_batched_benchmark(
          "file_write_mmap_flush", options, affinity_cpu,
          "mapped_memcpy_with_flush_view_and_file_buffers",
          [&](std::uint64_t operations) {
            return mapped_writer_flush.write_batch_flush(operations);
          }));
    }

    std::cout << "sink=" << g_io_benchmark_sink << "\n";
    for (const BenchmarkResult& result : results) {
      print_result(result);
    }

    write_csv(options.csv_path, results);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "io benchmark failed: " << ex.what() << "\n";
    return 1;
  }
}
