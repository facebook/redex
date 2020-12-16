/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <utility>

// Wrapper for a self-recursive lambda without the need for a std::function.
// The recursion actually happens through a "self" parameter in the lambda.
//
// Example:
//   self_recursive_fn([](auto self, int i) {
//     return i == 0 ? 1 : i == 1 ? 1 : self(i - 1) + self(i-2);
//   }, 3);
template <typename Fn, typename... Args>
auto self_recursive_fn(const Fn& fn, Args&&... args)
    -> decltype(fn(fn, std::forward<Args>(args)...)) {
  return fn(fn, std::forward<Args>(args)...);
}

// Simple guard to execute given function on destruction.
template <typename Fn>
struct ScopeGuard {
  Fn fn;
  explicit ScopeGuard(Fn fn) : fn(std::move(fn)) {}
  ~ScopeGuard() { fn(); }
};
template <typename Fn>
ScopeGuard<Fn> at_scope_exit(Fn fn) {
  return ScopeGuard<Fn>(std::move(fn));
}
