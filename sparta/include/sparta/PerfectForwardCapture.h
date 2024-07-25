/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <utility>

namespace sparta {
namespace fwd_impl {

template <typename T>
class by_value {
 public:
  template <typename U>
  explicit by_value(U&& x) : m_x{std::forward<U>(x)} {}

  auto& get() & { return m_x; }
  const auto& get() const& { return m_x; }
  auto get() && { return std::move(m_x); }

 private:
  T m_x;
};

template <typename T>
class by_ref {
 public:
  explicit by_ref(T& x) : m_x{x} {}

  auto& get() & { return m_x.get(); }
  const auto& get() const& { return m_x.get(); }
  auto get() && { return std::move(m_x.get()); }

 private:
  std::reference_wrapper<T> m_x;
};

} // namespace fwd_impl

// Unspecialized version: stores a `T` instance by value.
template <typename T>
struct fwd_capture_wrapper : fwd_impl::by_value<T> {
  using fwd_impl::by_value<T>::by_value;
};

// Specialized version: stores a `T` reference.
template <typename T>
struct fwd_capture_wrapper<T&> : fwd_impl::by_ref<T> {
  using fwd_impl::by_ref<T>::by_ref;
};

/**
 * Utility function to perfectly forward a universal reference captured in a
 * lambda. See
 * https://vittorioromeo.info/index/blog/capturing_perfectly_forwarded_objects_in_lambdas.html
 *
 * Example usage:
 * ```
 * template <typename T>
 * auto f(T x) {
 *   return [x = fwd_capture(std::forward<T>(x))](...) mutable {
 *     auto v = x.get();
 *     // ...
 *   };
 * }
 * ```
 */
template <typename T>
auto fwd_capture(T&& x) {
  return fwd_capture_wrapper<T>(std::forward<T>(x));
}

} // namespace sparta
