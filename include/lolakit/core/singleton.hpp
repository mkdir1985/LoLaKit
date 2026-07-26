#ifndef LOLAKIT_CORE_SINGLETON_HPP_
#define LOLAKIT_CORE_SINGLETON_HPP_

#include <atomic>
#include <mutex>

#include "lolakit/core/macros.hpp"

namespace lolakit {
namespace core {

// Process-lifetime singleton:
// - C++11 guarantees thread-safe initialization of function-local statics.
// - We intentionally keep the instance alive until process exit to avoid
//   static destruction order problems in long-running systems.
//
// Usage:
//   class RiskConfig final : public lolakit::core::Singleton<RiskConfig> {
//     friend class lolakit::core::Singleton<RiskConfig>;
//   private:
//     RiskConfig() = default;
//   };
template <typename T>
class Singleton {
 public:
  static T& instance() {
    static T* value = new T();
    return *value;
  }

  Singleton(const Singleton&) = delete;
  Singleton& operator=(const Singleton&) = delete;
  Singleton(Singleton&&) = delete;
  Singleton& operator=(Singleton&&) = delete;

 protected:
  Singleton() = default;
  ~Singleton() = default;
};

// Process-lifetime singleton with double-checked locking:
// - Fast path is one acquire load and one predicted branch.
// - Slow path constructs exactly once behind a mutex and publishes with release.
// - The instance is intentionally leaked to avoid static destruction order issues.
template <typename T>
class DclpSingleton {
 public:
  static T& instance() {
    T* value = instance_ptr().load(std::memory_order_acquire);
    if (LOLAKIT_LIKELY(value != nullptr)) {
      return *value;
    }
    return *initialize_slow_path();
  }

  DclpSingleton(const DclpSingleton&) = delete;
  DclpSingleton& operator=(const DclpSingleton&) = delete;
  DclpSingleton(DclpSingleton&&) = delete;
  DclpSingleton& operator=(DclpSingleton&&) = delete;

 protected:
  DclpSingleton() = default;
  ~DclpSingleton() = default;

 private:
  static LOLAKIT_NOINLINE T* initialize_slow_path() {
    std::lock_guard<std::mutex> lock(mutex());
    T* value = instance_ptr().load(std::memory_order_relaxed);
    if (value == nullptr) {
      value = new T();
      instance_ptr().store(value, std::memory_order_release);
    }
    return value;
  }

  static std::atomic<T*>& instance_ptr() {
    static std::atomic<T*> value{nullptr};
    return value;
  }

  static std::mutex& mutex() {
    static std::mutex value;
    return value;
  }
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_SINGLETON_HPP_
