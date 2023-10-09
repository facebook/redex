/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <unordered_set>

#include <sparta/AbstractSet.h>
#include <sparta/PatriciaTreeUtil.h>

namespace sparta {

template <typename Element,
          typename Hash = std::hash<Element>,
          typename Equal = std::equal_to<Element>>
class HashSet final : public AbstractSet<HashSet<Element, Hash, Equal>> {
 private:
  using StdUnorderedSet = std::unordered_set<Element, Hash, Equal>;

 public:
  // C++ container concept member types
  using value_type = Element;
  using iterator = typename StdUnorderedSet::const_iterator;
  using const_iterator = iterator;
  using difference_type = typename StdUnorderedSet::difference_type;
  using size_type = typename StdUnorderedSet::size_type;
  using const_reference = typename StdUnorderedSet::const_reference;
  using const_pointer = typename StdUnorderedSet::const_pointer;

  HashSet() = default;

  ~HashSet() = default;

  HashSet(const HashSet& other) {
    if (other.m_set) {
      m_set = std::make_unique<StdUnorderedSet>(*other.m_set);
    }
  }

  HashSet(HashSet&& other) noexcept = default;

  HashSet& operator=(const HashSet& other) {
    if (m_set) {
      if (other.m_set) {
        *m_set = *other.m_set;
      } else {
        m_set->clear();
      }
    } else if (other.m_set) {
      // Create a new unique pointer by calling the copy constructor of
      // `StdUnorderedSet`.
      m_set = std::make_unique<StdUnorderedSet>(*other.m_set);
    }
    return *this;
  }

  HashSet& operator=(HashSet&& other) noexcept = default;

  explicit HashSet(Element e) { insert(std::move(e)); }

  explicit HashSet(std::initializer_list<Element> l) {
    for (Element e : l) {
      insert(e);
    }
  }

  template <typename InputIterator>
  HashSet(InputIterator first, InputIterator last) {
    for (auto it = first; it != last; ++it) {
      insert(*it);
    }
  }

  bool empty() const { return m_set ? m_set->empty() : true; }

  size_t size() const { return m_set ? m_set->size() : 0; }

  size_t max_size() const { return s_empty_set.max_size(); }

  iterator begin() const { return const_set().begin(); }

  iterator end() const { return const_set().end(); }

  HashSet& insert(const Element& element) {
    mutable_set().emplace(element);
    return *this;
  }

  HashSet& insert(Element&& element) {
    mutable_set().emplace(std::move(element));
    return *this;
  }

  HashSet& remove(const Element& element) {
    if (m_set) {
      m_set->erase(element);
    }
    return *this;
  }

  bool contains(const Element& element) const {
    return m_set && m_set->count(element) > 0;
  }

  bool is_subset_of(const HashSet& other) const {
    if (size() > other.size()) {
      return false;
    }
    if (m_set) {
      for (const Element& e : *m_set) {
        if (other.m_set->count(e) == 0) {
          return false;
        }
      }
    }
    return true;
  }

  bool equals(const HashSet& other) const {
    return (size() == other.size()) && is_subset_of(other);
  }

  /*
   * If the set is a singleton, returns a pointer to the element.
   * Otherwise, returns nullptr.
   */
  const Element* singleton() const {
    if (m_set && m_set->size() == 1) {
      return *&m_set->begin();
    } else {
      return nullptr;
    }
  }

  void clear() {
    if (m_set) {
      m_set->clear();
    }
  }

  template <typename Predicate> // bool(const Element&)
  HashSet& filter(Predicate&& predicate) {
    if (m_set) {
      auto it = m_set->begin(), end = m_set->end();
      while (it != end) {
        if (!predicate(*it)) {
          it = m_set->erase(it);
        } else {
          ++it;
        }
      }
    }
    return *this;
  }

  /*
   * Visit all elements.
   * This does NOT allocate memory, unlike the iterators.
   */
  template <typename Visitor> // void(const Element&)
  void visit(Visitor&& visitor) const {
    if (m_set) {
      for (const auto& element : *m_set) {
        visitor(element);
      }
    }
  }

  HashSet& union_with(const HashSet& other) {
    if (other.m_set) {
      auto& this_set = mutable_set();
      for (const Element& e : *other.m_set) {
        this_set.insert(e);
      }
    }
    return *this;
  }

  HashSet& intersection_with(const HashSet& other) {
    if (!m_set) {
      return *this;
    }
    if (!other.m_set) {
      m_set->clear();
      return *this;
    }
    auto it = m_set->begin(), end = m_set->end();
    while (it != end) {
      if (other.m_set->count(*it) == 0) {
        it = m_set->erase(it);
      } else {
        ++it;
      }
    }
    return *this;
  }

  HashSet& difference_with(const HashSet& other) {
    if (!m_set || !other.m_set) {
      return *this;
    }
    auto it = m_set->begin(), end = m_set->end();
    while (it != end) {
      if (other.m_set->count(*it) > 0) {
        it = m_set->erase(it);
      } else {
        ++it;
      }
    }
    return *this;
  }

  friend std::ostream& operator<<(std::ostream& o, const HashSet& s) {
    o << "{";
    for (auto it = s.begin(); it != s.end(); ++it) {
      o << pt_util::deref(*it);
      if (std::next(it) != s.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  const StdUnorderedSet& const_set() const {
    return m_set ? *m_set : s_empty_set;
  }

  StdUnorderedSet& mutable_set() {
    if (!m_set) {
      m_set = std::make_unique<StdUnorderedSet>();
    }
    return *m_set;
  }

 private:
  // Use `std::unique_ptr` to keep the size of HashSet small.
  std::unique_ptr<StdUnorderedSet> m_set;

  static inline const StdUnorderedSet s_empty_set{};
};

} // namespace sparta
