/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

template <typename T>
// A convenient helper class for lazy initialization.
// This class is not thread-safe.
class Lazy {
 public:
  Lazy() = delete;
  Lazy(const Lazy&) = delete;
  Lazy& operator=(const Lazy&) = delete;
  explicit Lazy(const std::function<T()>& creator) : m_creator(creator) {}
  operator bool() { return !!m_value; }
  T& operator*() {
    init();
    return *m_value;
  }
  T* operator->() {
    init();
    return m_value.get();
  }

 private:
  std::function<T()> m_creator;
  std::unique_ptr<T> m_value;
  void init() {
    if (!m_value) {
      m_value = std::make_unique<T>(m_creator());
      // Release whatever memory is asssociated with creator
      m_creator = std::function<T()>();
    }
  }
};
