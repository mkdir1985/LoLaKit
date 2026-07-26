#ifndef LOLAKIT_CORE_NONCOPYABLE_HPP_
#define LOLAKIT_CORE_NONCOPYABLE_HPP_

namespace lolakit {
namespace core {

class NonCopyable {
 protected:
  NonCopyable() = default;
  ~NonCopyable() = default;

  NonCopyable(NonCopyable&&) = default;
  NonCopyable& operator=(NonCopyable&&) = default;

 public:
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;
};

class NonMovable {
 protected:
  NonMovable() = default;
  ~NonMovable() = default;

  NonMovable(const NonMovable&) = default;
  NonMovable& operator=(const NonMovable&) = default;

 public:
  NonMovable(NonMovable&&) = delete;
  NonMovable& operator=(NonMovable&&) = delete;
};

class NonCopyableNonMovable {
 protected:
  NonCopyableNonMovable() = default;
  ~NonCopyableNonMovable() = default;

 public:
  NonCopyableNonMovable(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable& operator=(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable(NonCopyableNonMovable&&) = delete;
  NonCopyableNonMovable& operator=(NonCopyableNonMovable&&) = delete;
};

}  // namespace core
}  // namespace lolakit

#endif  // LOLAKIT_CORE_NONCOPYABLE_HPP_
