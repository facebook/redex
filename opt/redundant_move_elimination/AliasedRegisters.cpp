/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AliasedRegisters.h"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/range/iterator_range.hpp>
#include <set>

// Implemented by a graph where nodes are Registers and edges are an alias
// between them.

void AliasedRegisters::make_aliased(RegisterValue r1, RegisterValue r2) {
  if (r1 != r2) {
    vertex_t v1 = find_or_create(r1);
    vertex_t v2 = find_or_create(r2);
    boost::add_edge(v1, v2, m_graph);
  }
}

void AliasedRegisters::break_alias(RegisterValue r) {
  const auto& v = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (v != end) {
    // clear removes all edges incident to r
    boost::clear_vertex(*v, m_graph);
  }
}

bool AliasedRegisters::are_aliases(RegisterValue r1, RegisterValue r2) {
  if (r1 == r2) {
    return true;
  }

  const auto& v1 = find(r1);
  const auto& v2 = find(r2);
  const auto& end = boost::vertices(m_graph).second;
  if (v1 == end || v2 == end) {
    // if either register is not in the graph, then
    // they cannot be aliases
    return false;
  }

  return path_exists(*v1, *v2);
}

const boost::range_detail::integer_iterator<AliasedRegisters::vertex_t>
AliasedRegisters::find(RegisterValue r) {
  const auto& iters = boost::vertices(m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    if (m_graph[*it] == r) {
      return it;
    }
  }
  return end;
}

AliasedRegisters::vertex_t AliasedRegisters::find_or_create(RegisterValue r) {
  const auto& it = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (it != end) {
    return *it;
  } else {
    return boost::add_vertex(r, m_graph);
  }
}

template <typename T>
bool contains(const std::set<T>& set, const T& val) {
  const auto& search = set.find(val);
  return search != set.end();
}

// return true if there exists a path from `start` to `end`
//
// Implemented with recursive DFS, stopping as soon as `end` is found
// (or traversing the entire graph then returning false)
bool AliasedRegisters::path_exists(AliasedRegisters::vertex_t start,
                                   AliasedRegisters::vertex_t end) const {
  std::set<AliasedRegisters::vertex_t> visited;
  const auto& graph = m_graph;
  std::function<bool(vertex_t)> recurse =
      [&recurse, &graph, &visited, end](AliasedRegisters::vertex_t v) {
        if (v == end) {
          return true;
        }
        visited.insert(v);
        const auto& adj_vertices = boost::adjacent_vertices(v, graph);
        for (auto it = adj_vertices.first; it != adj_vertices.second; ++it) {
          if (!contains(visited, *it) && recurse(*it)) {
            return true;
          }
        }
        return false;
      };
  return recurse(start);
}
