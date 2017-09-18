/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AliasedRegisters.h"

#include <algorithm>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/connected_components.hpp>
#pragma GCC diagnostic pop
#include <boost/optional.hpp>
#include <boost/range/iterator_range.hpp>
#include <limits>
#include <unordered_set>

// Implemented by an undirected graph where nodes are Registers and edges are an
// alias between them.

void AliasedRegisters::make_aliased(const RegisterValue& r1, const RegisterValue& r2) {
  if (r1 != r2) {
    vertex_t v1 = find_or_create(r1);
    vertex_t v2 = find_or_create(r2);
    boost::add_edge(v1, v2, m_graph);
    invalidate_cache();
  }
}

void AliasedRegisters::break_alias(const RegisterValue& r) {
  const auto& v = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (v != end) {
    // clear removes all edges incident to r
    boost::clear_vertex(*v, m_graph);
    invalidate_cache();
  }
}

bool AliasedRegisters::are_aliases(const RegisterValue& r1, const RegisterValue& r2) {
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

template <typename T>
bool contains(const std::unordered_set<T>& set, const T& val) {
  return set.count(val) > 0;
}

/**
 * Return a representative for this register.
 *
 * Return the lowest numbered register that this value is an alias with.
 */
boost::optional<Register> AliasedRegisters::get_representative(
    const RegisterValue& r) {

  // if r is not in the graph, then it has no representative
  const auto& v = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (v == end) {
    return boost::none;
  }

  // compute connected components
  // (or use cached value when graph hasn't changed)
  auto num_vertices = boost::num_vertices(m_graph);
  if (m_conn_components.empty()) {
    m_conn_components.resize(num_vertices);
    boost::connected_components(m_graph, m_conn_components.data());
  }
  int component_of_v = m_conn_components.at(*v);

  // find the lowest numbered register in the same component as `v`
  Register result = std::numeric_limits<Register>::max();
  for (vertex_t candidate = 0; candidate < num_vertices; ++candidate) {
    const RegisterValue& val = m_graph[candidate];
    if (component_of_v == m_conn_components.at(candidate) &&
        val.kind == RegisterValue::Kind::REGISTER) {
      result = std::min<Register>(result, val.reg);
    }
  }
  if (result == std::numeric_limits<Register>::max()) {
    return boost::none;
  }
  return result;
}

const boost::range_detail::integer_iterator<AliasedRegisters::vertex_t>
AliasedRegisters::find(const RegisterValue& r) const {
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

// returns the vertex holding `r` or creates a new (unconnected)
// vertex if `r` is not in m_graph
AliasedRegisters::vertex_t AliasedRegisters::find_or_create(const RegisterValue& r) {
  const auto& it = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (it != end) {
    return *it;
  } else {
    invalidate_cache();
    return boost::add_vertex(r, m_graph);
  }
}

// return true if there exists a path from `start` to `end`
//
// Implemented with recursive DFS, stopping as soon as `end` is found
// (or traversing the entire graph then returning false)
bool AliasedRegisters::path_exists(AliasedRegisters::vertex_t start,
                                   AliasedRegisters::vertex_t end) const {
  std::unordered_set<AliasedRegisters::vertex_t> visited;
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

bool AliasedRegisters::has_edge_between(const RegisterValue& r1,
                                        const RegisterValue& r2) const {
  // make sure we have both vertices
  const auto& search1 = find(r1);
  const auto& search2 = find(r2);
  const auto& end = boost::vertices(m_graph).second;
  if (search1 == end || search2 == end) {
    return false;
  }

  // and check that they have an edge between them
  const auto& adj = boost::adjacent_vertices(*search1, m_graph);
  const auto& adj_begin = adj.first;
  const auto& adj_end = adj.second;
  const auto& edge_search = std::find(adj_begin, adj_end, *search2);
  if (edge_search == adj_end) {
    return false;
  }
  return true;
}

// call this when m_graph changes
void AliasedRegisters::invalidate_cache() { m_conn_components.clear(); }

// ---- extends AbstractValue ----

void AliasedRegisters::clear() {
  m_graph.clear();
  invalidate_cache();
}

AliasedRegisters::Kind AliasedRegisters::kind() const {
  return (boost::num_edges(m_graph) > 0) ? AliasedRegisters::Kind::Value
                                         : AliasedRegisters::Kind::Top;
}

/**
 * The lattice looks like this:
 *
 *             T (graphs with no edges)
 *      graphs with 1 edge                  ^  join moves up (edge intersection)
 *      graphs with 2 edges                 |
 *            ...                           v  meet moves down (edge union)
 *      graphs with n edges
 *            ...
 *            _|_
 *
 * So, leq is the superset relation on the edge set
 */
bool AliasedRegisters::leq(const AliasedRegisters& other) const {
  if (boost::num_edges(m_graph) < boost::num_edges(other.m_graph)) {
    // this cannot be a superset of other if this has fewer edges
    return false;
  }

  // for all edges in other (the potential subset), make sure this contains that
  // edge.
  const auto& iters = boost::edges(other.m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    const RegisterValue& r1 = other.m_graph[boost::source(*it, other.m_graph)];
    const RegisterValue& r2 = other.m_graph[boost::target(*it, other.m_graph)];
    if (!has_edge_between(r1, r2)) {
      return false;
    }
  }
  return true;
}

/*
 * returns true iff they have exactly the same edges between the same
 * RegisterValues
 */
bool AliasedRegisters::equals(const AliasedRegisters& other) const {
  return boost::num_edges(m_graph) == boost::num_edges(other.m_graph) &&
         leq(other);
}

// edge union
AliasedRegisters::Kind AliasedRegisters::meet_with(
    const AliasedRegisters& other) {
  const auto& iters = boost::edges(other.m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    const RegisterValue& r1 = other.m_graph[boost::source(*it, other.m_graph)];
    const RegisterValue& r2 = other.m_graph[boost::target(*it, other.m_graph)];
    this->make_aliased(r1, r2);
  }
  this->invalidate_cache();
  return AliasedRegisters::Kind::Value;
}

AliasedRegisters::Kind AliasedRegisters::narrow_with(
    const AliasedRegisters& other) {
  return join_with(other);
}

// edge intersection
AliasedRegisters::Kind AliasedRegisters::join_with(
    const AliasedRegisters& other) {

  // fill `deletes` with edges that aren't in `other`
  std::vector<std::pair<vertex_t, vertex_t>> deletes;
  const auto& iters = boost::edges(this->m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    vertex_t v1 = boost::source(*it, this->m_graph);
    vertex_t v2 = boost::target(*it, this->m_graph);
    const RegisterValue& r1 = this->m_graph[v1];
    const RegisterValue& r2 = this->m_graph[v2];
    if (!other.has_edge_between(r1, r2)) {
      deletes.emplace_back(v1, v2);
    }
  }

  for (const auto& edge : deletes) {
    boost::remove_edge(edge.first, edge.second, this->m_graph);
  }

  this->invalidate_cache();
  return AliasedRegisters::Kind::Value;
}

AliasedRegisters::Kind AliasedRegisters::widen_with(
    const AliasedRegisters& other) {
  return meet_with(other);
}
