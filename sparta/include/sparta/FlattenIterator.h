/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iterator>
#include <optional>
#include <type_traits>

namespace sparta {

namespace fi_impl {

template <typename T>
struct Range {
  T begin;
  T end;
};

template <typename OuterIterator>
struct FlattenDereference {
  using Reference = typename std::iterator_traits<OuterIterator>::reference;
  using InnerIterator = decltype(std::declval<Reference>().begin());

  static InnerIterator begin(Reference reference) { return reference.begin(); }

  static InnerIterator end(Reference reference) { return reference.end(); }
};

template <typename OuterIterator>
struct FlattenConstDereference {
  using Reference = typename std::iterator_traits<OuterIterator>::reference;
  using InnerIterator = decltype(std::declval<Reference>().cbegin());

  static InnerIterator begin(Reference reference) { return reference.cbegin(); }

  static InnerIterator end(Reference reference) { return reference.cend(); }
};

} // namespace fi_impl

/**
 * A flattening iterator that iterates on a container of containers.
 *
 * For instance, this can be used to treat a `std::vector<std::vector<T>>` as
 * a single list of `T`.
 */
template <typename OuterIterator,
          typename InnerIterator,
          typename Dereference = fi_impl::FlattenDereference<OuterIterator>>
class FlattenIterator {
 public:
  using OuterReference =
      typename std::iterator_traits<OuterIterator>::reference;

  static_assert(std::is_same_v<
                decltype(Dereference::begin(std::declval<OuterReference>())),
                InnerIterator>);
  static_assert(
      std::is_same_v<decltype(Dereference::end(std::declval<OuterReference>())),
                     InnerIterator>);

  // C++ iterator concept member types
  using iterator_category = std::forward_iterator_tag;
  using value_type = typename std::iterator_traits<InnerIterator>::value_type;
  using difference_type =
      typename std::iterator_traits<OuterIterator>::difference_type;
  using pointer = typename std::iterator_traits<InnerIterator>::pointer;
  using reference = typename std::iterator_traits<InnerIterator>::reference;

  explicit FlattenIterator(OuterIterator begin, OuterIterator end)
      : m_outer(
            fi_impl::Range<OuterIterator>{std::move(begin), std::move(end)}),
        m_inner(std::nullopt) {
    if (m_outer.begin == m_outer.end) {
      return;
    }
    m_inner = fi_impl::Range<InnerIterator>{Dereference::begin(*m_outer.begin),
                                            Dereference::end(*m_outer.begin)};
    advance_empty();
  }

  FlattenIterator& operator++() {
    ++m_inner->begin;
    advance_empty();
    return *this;
  }

  FlattenIterator operator++(int) {
    FlattenIterator result = *this;
    ++(*this);
    return result;
  }

  bool operator==(const FlattenIterator& other) const {
    return m_outer.begin == other.m_outer.begin &&
           ((!m_inner.has_value() && !other.m_inner.has_value()) ||
            (m_inner.has_value() && other.m_inner.has_value() &&
             m_inner->begin == other.m_inner->begin));
  }

  bool operator!=(const FlattenIterator& other) const {
    return !(*this == other);
  }

  reference operator*() { return *m_inner->begin; }

  pointer operator->() const { return &*m_inner->begin; }

 private:
  /* Advance the iterator until we find an element. */
  void advance_empty() {
    while (m_inner->begin == m_inner->end) {
      ++m_outer.begin;
      if (m_outer.begin == m_outer.end) {
        m_inner = std::nullopt;
        return;
      } else {
        m_inner =
            fi_impl::Range<InnerIterator>{Dereference::begin(*m_outer.begin),
                                          Dereference::end(*m_outer.begin)};
      }
    }
  }

  fi_impl::Range<OuterIterator> m_outer;
  std::optional<fi_impl::Range<InnerIterator>> m_inner;
};

} // namespace sparta
