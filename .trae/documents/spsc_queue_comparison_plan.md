# SPSC 队列对比测试 实现计划

## 仓库调研结论

### 现有架构与风格
- **Header-only 核心库**（`include/lolakit/core/`），C++17，MSVC 友好
- **已有 SPSC 无锁队列**：[spsc_ring_buffer.hpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/include/lolakit/core/spsc_ring_buffer.hpp) 提供两个实现：
  - `SpscRingBuffer<T, Capacity>`：通用版本，支持非平凡类型，手动 placement new / 显式析构
  - `TrivialSpscRingBuffer<T, Capacity>`：平凡类型优化版，零开销直接赋值
  - **底层存储**：栈上固定数组 `Slot slots_[Capacity]`，编译期容量，CacheLine 隔离生产/消费索引
- **基础设施齐全**：[atomic.hpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/include/lolakit/core/atomic.hpp) 内存序封装、[cacheline.hpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/include/lolakit/core/cacheline.hpp) 对齐填充、[spin_wait.hpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/include/lolakit/core/spin_wait.hpp) 自旋退避、[noncopyable.hpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/include/lolakit/core/noncopyable.hpp) 基类
- **单元测试框架**：[core_unit_tests.cpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/tests/core_unit_tests.cpp) 自定义轻量框架，已有 SPSC 的：容量 FIFO、wraparound clear、双线程往返 3 个测试用例
- **Benchmark 框架**：[core_benchmark.cpp](file:///c:/Users/mkdir/Desktop/source/mkdir/LoLaKit/benchmarks/core_benchmark.cpp) 支持 warmup / batch / CPU affinity / 百分位统计 / CSV 输出；已有 `run_spsc_two_thread_benchmark` 双线程端到端基准

### 约束与规范
- 容量要求：2 的幂（现有无锁实现依赖 `& kIndexMask 取模优化）
- 接口约定：`try_push(const T&) / try_push(T&&) / emplace(...) / try_pop(T&) / try_pop() -> optional<T> / empty() / full() / size() / clear() / capacity()`
- 风格：无异常接口加 `noexcept`、析构安全、继承 `NonCopyableNonMovable`
- 构建：CMakeLists.txt 无需新 target，测试文件直接加入现有可执行文件

## Files and Modules

### 新增文件
- `include/lolakit/core/mutex_vector_spsc_queue.hpp`：**有锁版本 SPSC 队列（mutex + vector 环形缓冲）
- `include/lolakit/core/vector_spsc_ring_buffer.hpp`：**无锁版本 SPSC 队列（vector 作为底层存储的堆分配版本）

### 修改文件
- `include/lolakit/core.hpp`：追加两个新头文件 include
- `tests/core_unit_tests.cpp`：追加新队列的单元测试（容量 FIFO、wraparound、双线程 roundtrip）
- `benchmarks/core_benchmark.cpp`：追加对比 benchmark（单线程往返、双线程端到端）

## 实现步骤

1. **实现 `MutexVectorSpscQueue<T, Capacity>`（core 新增）**
   - 成员：`std::mutex mutex_` + `std::vector<Slot> slots_`（大小为 Capacity）
   - 索引：`std::size_t producer_` / `consumer_` 普通下标（无需 atomic，有锁保护）
   - 接口：与 `SpscRingBuffer` 完全一致的 try_push/emplace/try_pop/empty/full/size/clear
   - 语义：满时 `try_push` 返回 false，空时 `try_pop` 返回 false，阻塞由调用方自旋
   - 平凡/非平凡类型分开实现，Slot 结构复用现有模式（placement new + 显式析构）

2. **实现 `VectorSpscRingBuffer<T, Capacity>`（core 新增）**
   - 将现有 `SpscRingBuffer` 的 `Slot slots_[Capacity]` 改为 `std::vector<Slot> slots_`，构造时 `resize(Capacity)`
   - 其余逻辑（索引缓存、cacheline 隔离、内存序）完全保留 `SpscRingBuffer` 的设计
   - 同步实现 `TrivialVectorSpscRingBuffer` 平凡优化版

3. **更新 `core.hpp` 聚合头**
   - `#include "lolakit/core/mutex_vector_spsc_queue.hpp"`
   - `#include "lolakit/core/vector_spsc_ring_buffer.hpp"`

4. **单元测试（tests/core_unit_tests.cpp 追加）**
   对 `MutexVectorSpscQueue` 和 `VectorSpscRingBuffer` 各追加：
   - `test_mutex_vector_spsc_capacity_and_fifo`：满/空/FIFO 顺序/满 push 失败/空 pop 失败
   - `test_mutex_vector_spsc_wraparound_and_clear`：回绕 + `TrackedValue` 析构检查
   - `test_mutex_vector_spsc_threaded_roundtrip`：200k 消息双线程，校验和校验
   （无锁 vector 版本相同的相应测试函数

5. **Benchmark（benchmarks/core_benchmark.cpp 追加）**
   - 单线程往返：`mutex_vector_spsc_single_thread`、`vector_spsc_single_thread`
   - 双线程端到端：新增 `run_mutex_vector_spsc_two_thread_benchmark`、`run_vector_spsc_two_thread_benchmark`
   - 复用现有 `run_batched_benchmark` 和 `run_spsc_two_thread_benchmark` 的采样、统计、CSV 逻辑
   - 对比维度：ns/op、p50/p95/p99、Mops/s

## Dependencies and Considerations
- **公平对比前提**：4 种实现（含现有 `SpscRingBuffer`）使用相同 Capacity=1024、相同消息量、相同 affinity 策略（如 `--affinity-cpu 0` 则 producer=cpu0 / consumer=cpu1）
- **vector 存储差异**：数组 vs vector 仅是栈/堆分配差异，无锁算法核心不变
- **mutex 版本无需缓存消费者索引**：有锁下直接读原子即可，无需缓存
- **类型匹配**：Slot 结构、kIndexMask、容量约束（2 的幂）保持一致
- **MSVC `/wd4324` 已全局忽略结构对齐警告，新增 CacheLinePadded 成员无需额外处理

## 验证
- 构建：CMake configure + build，无编译错误无警告
- 单元测试：`ctest -R lolakit_core_unit_tests` 全部通过
- 手动运行 benchmark：`lolakit_core_benchmark --iterations 1000000 --csv out.csv`，观察 4 种实现的 ns/op 对比，mutex 预期比无锁慢 5~20 倍（视竞争程度）
- 类型安全：`TrackedValue::live_count` 归零，无内存泄漏

## Risks
- **mutex 版本在高吞吐场景下 OS 调度导致尾延迟飙升：正常现象，benchmark 如实记录 p99 即可
- **vector 构造 resize(Capacity) 未初始化对象**：Slot 使用 `alignas(T) unsigned char storage[sizeof(T)]` 与原实现一致，不触发 T 的默认构造，安全
- **双线程 benchmark 的 batch_start / batch_end 的计时归属：生产开始在生产线程，消费结束在消费线程，端到端延迟准确；两套实现采用完全相同计时方式，可对比
- **Windows 下 mutex 内核对象开销：结论中需注明 mutex 含内核态切换成本
