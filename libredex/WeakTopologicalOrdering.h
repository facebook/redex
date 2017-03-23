/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stack>
#include <vector>

#include "Debug.h"

template <typename NodeId>
class WtoComponent;
template <typename NodeId>
class WeakTopologicalOrdering;

namespace wto_impl {

/*
 * Iterator over the subcomponents of a strongly connected component (head
 * node excluded). This is a regular C++ iterator meant for traversing a
 * strongly connected component. It's not a fixpoint iterator.
 */
template <typename NodeId>
class WtoComponentIterator final
    : public std::iterator<std::forward_iterator_tag, WtoComponent<NodeId>> {
 public:
  WtoComponentIterator& operator++() {
    assert(m_component != nullptr);
    if (m_component->m_next_component_offset == 0) {
      // All components of a WTO are stored linearly inside a vector in reverse
      // order. The subcomponents of an SCC are stored between the head node and
      // the next component in the WTO. Hence, if the offset of the next
      // component is 0, it means that the SCC has no subcomponents aside from
      // its head node.
      m_component = nullptr;
    } else {
      m_component -= m_component->m_next_component_offset;
    }
    return *this;
  }

  WtoComponentIterator operator++(int) {
    WtoComponentIterator retval = *this;
    ++(*this);
    return retval;
  }

  bool operator==(WtoComponentIterator other) const {
    return m_component == other.m_component;
  }

  bool operator!=(WtoComponentIterator other) const {
    return !(*this == other);
  }

  const WtoComponent<NodeId>& operator*() {
    assert(m_component != nullptr);
    return *m_component;
  }

  const WtoComponent<NodeId>* operator->() {
    assert(m_component != nullptr);
    return m_component;
  }

 private:
  WtoComponentIterator(const WtoComponent<NodeId>* component)
      : m_component(component) {}

  const WtoComponent<NodeId>* m_component;

  template <typename T>
  friend class ::WtoComponent;
  template <typename T>
  friend class ::WeakTopologicalOrdering;
};

} // namespace wto_impl

/*
 * A component of a weak topological ordering is either a vertex or a strongly
 * connected set of nodes with a distinguished node (the head).
 */
template <typename NodeId>
class WtoComponent final {
 public:
  using iterator = wto_impl::WtoComponentIterator<NodeId>;

  enum class Kind { Vertex, Scc };

  /*
   * If the component is not strongly connected, this method returns the single
   * node contained inside a Vertex component.
   */
  NodeId head_node() const { return m_node; }

  bool is_vertex() const { return m_kind == Kind::Vertex; }

  bool is_scc() const { return m_kind == Kind::Scc; }

  iterator begin() const {
    assert(is_scc());
    if (m_next_component_offset == 1) {
      return end();
    }
    // All the components of a WTO are stored linearly inside a vector. A vector
    // guarantees that all its elements are stored adjacently in a contiguous
    // block of memory, which allows us to safely perform pointer arithmetic
    // based on the 'this' pointer (the vector is never resized after the WTO
    // has been constructed).
    return iterator(this - 1);
  }

  iterator end() const {
    assert(is_scc());
    return iterator(nullptr);
  }

  WtoComponent(NodeId node,
               Kind kind,
               int32_t position,
               int32_t next_component_position)
      : m_node(node), m_kind(kind) {
    assert(position > next_component_position);
    // When a component is constructed, its position inside the vector is
    // specified by its absolute index. Since we want to navigate the WTO by
    // recursively exploring SCCs, it's more efficient to maintain relative
    // offsets between adjacent components. An absolute position of -1 means
    // the end of an SCC or the end of the WTO.
    m_next_component_offset = (next_component_position == -1)
                                  ? 0
                                  : position - next_component_position;
  }

 private:
  NodeId m_node;
  Kind m_kind;
  size_t m_next_component_offset;

  template <typename T>
  friend class wto_impl::WtoComponentIterator;
  template <typename T>
  friend class WeakTopologicalOrdering;
};

/*
 * Implementation of the decomposition of a rooted directed graph into a weak
 * topological ordering (WTO), as described in Bourdoncle's original paper:
 *   F. Bourdoncle. Efficient chaotic iteration strategies with widenings.
 *   In Formal Methods in Programming and Their Applications, pp 128-141.
 * State-of-the-art fixpoint iteration algorithms use weak topological orderings
 * as the underlying structure for high performance. Although we will primarily
 * use WTOs on the control-flow graph of a FatMethod, WTOs can come handy when
 * manipulating structures like call graphs or dependency graphs, hence the
 * parametric class definition. This also makes the design of unit tests much
 * easier.
 *
 * - NodeId is the identifier of a node in the graph. It's meant to be a simple
 *   type like an int, a pointer or a string.
 */
template <typename NodeId>
class WeakTopologicalOrdering final {
 public:
  using iterator = wto_impl::WtoComponentIterator<NodeId>;

  /*
   * In order to construct a WTO, we just need to specify the root of the graph
   * and the successor function.
   */
  WeakTopologicalOrdering(NodeId root,
                          std::function<std::vector<NodeId>(NodeId)> successors)
      : m_successors(successors), m_free_position(0), m_num(0) {
    int32_t partition = -1;
    visit(root, &partition);
  }

  iterator begin() const { return iterator(&m_wto_space.back()); }

  iterator end() const { return iterator(nullptr); }

 private:
  // We keep the notations used by Bourdoncle in the paper to describe the
  // algorithm.
  uint32_t visit(NodeId vertex, int32_t* partition) {
    m_stack.push(vertex);
    int32_t head = set_dfn(vertex, ++m_num);
    bool loop = false;
    for (NodeId succ : m_successors(vertex)) {
      uint32_t succ_dfn = get_dfn(succ);
      uint32_t min = (succ_dfn == 0) ? visit(succ, partition) : succ_dfn;
      if (min <= head) {
        head = min;
        loop = true;
      }
    }
    if (head == get_dfn(vertex)) {
      // We encode the special value +oo used in the paper with UINT32_MAX.
      set_dfn(vertex, std::numeric_limits<uint32_t>::max());
      NodeId element = m_stack.top();
      m_stack.pop();
      if (loop) {
        while (element != vertex) {
          set_dfn(element, 0);
          element = m_stack.top();
          m_stack.pop();
        }
        push_component(vertex);
      }
      auto kind = loop ? WtoComponent<NodeId>::Kind::Scc
                       : WtoComponent<NodeId>::Kind::Vertex;
      m_wto_space.emplace_back(vertex, kind, m_free_position, *partition);
      *partition = m_free_position++;
    }
    return head;
  }

  void push_component(NodeId vertex) {
    int32_t partition = -1;
    for (NodeId succ : m_successors(vertex)) {
      if (get_dfn(succ) == 0) {
        visit(succ, &partition);
      }
    }
  }

  uint32_t get_dfn(NodeId node) {
    auto it = m_dfn.find(node);
    if (it != m_dfn.end()) {
      return it->second;
    }
    return 0;
  }

  uint32_t set_dfn(NodeId node, uint32_t number) {
    if (number == 0) {
      m_dfn.erase(node);
    } else {
      m_dfn[node] = number;
    }
    return number;
  }

  std::function<std::vector<NodeId>(NodeId)> m_successors;
  // We store all the components of a WTO inside a vector. This is more
  // efficient than allocating each component individually on the heap.
  // It's also more cache-friendly when repeatedly traversing the WTO during
  // a fixpoint iteration.
  std::vector<WtoComponent<NodeId>> m_wto_space;
  // The next available position at the end of the vector m_wto_space.
  int32_t m_free_position;
  // These are auxiliary data structures used by Bourdoncle's algorithm.
  std::unordered_map<NodeId, uint32_t> m_dfn;
  std::stack<NodeId> m_stack;
  uint32_t m_num;
};

template <typename NodeId>
inline std::ostream& operator<<(std::ostream& o,
                                const WtoComponent<NodeId>& c) {
  if (c.is_scc()) {
    o << "(" << c.head_node();
    for (const WtoComponent<NodeId>& sub : c) {
      o << " " << sub;
    }
    o << ")";
  } else {
    o << c.head_node();
  }
  return o;
}

template <typename NodeId>
inline std::ostream& operator<<(std::ostream& o,
                                const WeakTopologicalOrdering<NodeId>& wto) {
  for (auto it = wto.begin(); it != wto.end();) {
    o << *it++;
    if (it != wto.end()) {
      o << " ";
    }
  }
  return o;
}
