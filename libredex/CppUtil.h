/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cctype>
#include <string_view>
#include <utility>

// Wrapper for a self-recursive lambda without the need for a std::function.
// The recursion actually happens through a "self" parameter in the lambda.
//
// Example:
//   self_recursive_fn([](auto self, int i) {
//     return i == 0 ? 1 : i == 1 ? 1 : self(self, i - 1) + self(self, i-2);
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

// In languages like Java, it's quite common to use String.split("delim") to
// perform some basic parsing. For example, CSV files. StringSplitterIterator
// performs the split without requiring copying over string contents.

class StringSplitterIterator {
 public:
  StringSplitterIterator(const std::string_view& str,
                         const std::string_view& delim)
      : StringSplitterIterator(str, delim, str) {}

  std::string_view operator*() { return m_current_element; }
  StringSplitterIterator& operator++() { return next(); }
  StringSplitterIterator& next() {
    *this = StringSplitterIterator(m_str, m_delim, m_remaining, m_has_next);
    return *this;
  }

  // O(1) comparing of iterators.
  // Users MUSTN'T compare StringSplitterIterators constructed from different
  // str and delim.
  bool operator==(const StringSplitterIterator& other) const {
    return m_current_element.data() == other.m_current_element.data() &&
           m_has_this == other.m_has_this;
  }

  bool operator!=(const StringSplitterIterator& other) const {
    return !(*this == other);
  }

  StringSplitterIterator begin() {
    return StringSplitterIterator(m_str, m_delim, m_str);
  }

  StringSplitterIterator end() {
    return StringSplitterIterator(
        m_str, m_delim,
        std::string_view(m_remaining.data() + m_remaining.size()), false);
  }

 private:
  std::string_view m_str; // Used to construct begin()
  std::string_view m_delim;
  std::string_view m_remaining;
  std::string_view m_current_element;
  bool m_has_this; // True when this iterator is not end().
  bool m_has_next; // If the string ends with delimiter, there is another
                   // empty string as the end(). When reaching the end of the
                   // string, use this flag to tell whether the next empty
                   // string should be an element.

  StringSplitterIterator(std::string_view str,
                         std::string_view delim,
                         std::string_view remaining,
                         bool has_this = true)
      : m_str(str),
        m_delim(delim),
        m_remaining(remaining),
        m_has_this(has_this),
        m_has_next(has_this) {

    auto find_pos = m_remaining.find(m_delim);
    m_current_element = m_remaining.substr(
        0, find_pos); // Should also work when find_pos == npos.

    m_remaining.remove_prefix(m_current_element.size());
    if (find_pos != std::string_view::npos) {
      m_remaining.remove_prefix(m_delim.size());
    } else {
      m_has_next = false;
    }
  }
};

struct StringSplitterIterable {
 public:
  using iterator = StringSplitterIterator;
  StringSplitterIterable(const iterator& begin, const iterator& end)
      : m_begin(begin), m_end(end) {}
  iterator begin() const { return m_begin; }
  iterator end() const { return m_end; }

 private:
  iterator m_begin;
  iterator m_end;
};

// Returns a pair of iterators. Delimiter MUST be nonempty.
inline StringSplitterIterable split_string(std::string_view str,
                                           std::string_view delim) {
  StringSplitterIterator ss(str, delim);
  return StringSplitterIterable(ss /* begin */, ss.end());
}

inline std::string_view trim_whitespaces(std::string_view str) {
  if (str.empty()) {
    return str;
  }
  while (isspace(str.front())) {
    str.remove_prefix(1);
  }
  while (isspace(str.back())) {
    str.remove_suffix(1);
  }
  return str;
}
