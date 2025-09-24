/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>

/**
 * A thread-safe counter that can be atomically incremented.
 *
 * This is preferred over using std::mutex or std::atomic<T> directly because:
 *
 * - This class is lock free and hence doesn't bare the cost of
 *   locking/unlocking a mutex.
 * - Most atomic operations in std::atomic<T> (if not used carefully) and
 *   locking/unlocking in std::mutex incurs a memory fence, which is unnecessary
 *   and expensive for stat counting purposes.
 * - It's easier to maintain.
 *
 * You shouldn't use this class if your stat counting is single-threaded. Even
 * though this class is lock-free, atomic operations still bear extra cost,
 * which is unnecessary in single-threaded scenarios. For multi-threaded stat
 * counting, it's usually more efficient than this class to use the walker
 * pattern, where each thread count stats separately, which then get sumed up
 * after joining threads.
 *
 * Its APIs follow the same semantics as those of std::atomic<T>. But they
 * differ in some ways:
 *
 * - This is intended to be used as a counter, so it only works with integers.
 * - All APIs are memory order relaxed. This is because the counter only needs
 *   to synchronize itself among threads, and the overhead of memory is
 *   undesirable.
 * - It doesn't overload operator=(T) because it introduces many ways to
 *   accidentally use it non-atomically, such as `counter = counter + 1`.
 */
template <typename T>
class AtomicStatCounter final {
  using AtomicCounterType = std::atomic<T>;

  static_assert(std::is_integral<T>::value, "T must be an integral type");
  static_assert(AtomicCounterType::is_always_lock_free,
                "T must be always lock free. Use a mutex instead.");

  AtomicCounterType counter;

 public:
  // Require providing explicit value in construction to enforce better clarity.
  AtomicStatCounter() = delete;
  explicit AtomicStatCounter(T value) noexcept : counter{value} {}
  AtomicStatCounter(const AtomicStatCounter& other) noexcept
      : counter{other.counter.load(std::memory_order_relaxed)} {}
  AtomicStatCounter(AtomicStatCounter&& other) noexcept
      // There's no more efficient way to move an atomic than to copy it.
      // NOLINTNEXTLINE(performance-move-constructor-init)
      : AtomicStatCounter(other) // invoke the copy constructor
  {}
  AtomicStatCounter& operator=(const AtomicStatCounter& other) noexcept {
    counter.store(other.counter.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
    return *this;
  }
  AtomicStatCounter& operator=(AtomicStatCounter&& other) noexcept {
    *this = other; // invoke the copy assignment operator
    return *this;
  }

  T load() const noexcept { return counter.load(std::memory_order_relaxed); }
  // NOLINTNEXTLINE(google-explicit-constructor)
  operator T() const noexcept { return load(); }
  T operator++() noexcept { return *this += 1; }
  T operator++(int) noexcept {
    return counter.fetch_add(1, std::memory_order_relaxed);
  }
  T operator+=(T value) noexcept {
    counter.fetch_add(value, std::memory_order_relaxed);
    return *this;
  }
};
