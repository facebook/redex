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
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <boost/graph/adjacency_list.hpp>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <boost/optional.hpp>
#include <boost/range/iterator_range.hpp>
#include <limits>
#include <unordered_set>

// Implemented by an undirected graph where nodes are Registers and edges are an
// alias between them.
//
// An alias group is a fully connected clique of nodes.
// Every node in a group is aliased to every other node.
//
// Data structure invariant: The graph is a forest of cliques
// Corollary: There are no edges that are not part of a clique.
//
// The aliasing relation is an equivalence relation. An alias group is an
// equivalence class of this relation.
//   Reflexive : a node is trivially equivalent to itself
//   Symmetric : edges in the graph are undirected
//   Transitive: `AliasedRegisters::move` adds an edge to every node in the
//               group, creating a clique

namespace aliased_registers {

// Move `moving` into the alias group of `group`
//
// Create an edge from `moving` to every vertex in the alias group of
// `group`.
//
// We want alias groups to be fully connected cliques.
// Here's an example to show why:
//
//   move v1, v2
//   move v0, v1 # (call `AliasedRegisters::move(v0, v1)` here)
//   const v1, 0
//
// At this point, v0 and v2 still hold the same value, but if we had just
// `add_edge(v0, v1)`, then we would have lost this information.
void AliasedRegisters::move(const Value& moving,
                            const Value& group) {
  // Only need to do something if they're not already in same group
  if (!are_aliases(moving, group)) {
    // remove from the old group
    break_alias(moving);
    vertex_t mov = find_or_create(moving);
    vertex_t grp = find_or_create(group);
    // add edge to every node in new group
    for (vertex_t v : vertices_in_group(grp)) {
      boost::add_edge(mov, v, m_graph);
    }
  }
}

// This method is only public for testing reasons. Only use it if you know what
// you're doing. This method does not maintain transitive closure of the graph,
// You probably want to use `move`
void AliasedRegisters::add_edge(const Value& r1, const Value& r2) {
  if (r1 != r2) {
    vertex_t v1 = find_or_create(r1);
    vertex_t v2 = find_or_create(r2);
    boost::add_edge(v1, v2, m_graph);
  }
}

// Remove r from its alias group
void AliasedRegisters::break_alias(const Value& r) {
  const auto& v = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (v != end) {
    // clear removes all edges incident to r
    boost::clear_vertex(*v, m_graph);
  }
}

// if there is an edge between r1 to r2, then they are aliases.
// We only need to check for single edge paths because `move` adds an edge to
// every node in the alias group, thus maintaining transitive closure of the
// graph
bool AliasedRegisters::are_aliases(const Value& r1, const Value& r2) {
  if (r1 == r2) {
    return true;
  }

  return has_edge_between(r1, r2);
}

// Return a representative for this register.
//
// Return the lowest numbered register that this value is an alias with.
Register AliasedRegisters::get_representative(
    const Value& r) {
  always_assert(r.is_register());

  // if r is not in the graph, then it has no representative
  const auto& v = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (v == end) {
    return r.reg();
  }

  // find the lowest numbered register in the same alias group as `v`
  boost::optional<Register> representative = boost::none;
  for (vertex_t candidate : vertices_in_group(*v)) {
    const Value& val = m_graph[candidate];
    if (val.is_register()) {
      if (!representative) {
        representative = val.reg();
      } else {
        representative = std::min<Register>(*representative, val.reg());
      }
    }
  }
  return representative == boost::none ? r.reg() : *representative;
}

// if `r` is in the graph, return the vertex holding it.
// if not, return the `end` iterator of the vertices
const boost::range_detail::integer_iterator<AliasedRegisters::vertex_t>
AliasedRegisters::find(const Value& r) const {
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
AliasedRegisters::vertex_t AliasedRegisters::find_or_create(const Value& r) {
  const auto& it = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (it != end) {
    return *it;
  } else {
    return boost::add_vertex(r, m_graph);
  }
}

// return true if there is a path of length exactly 1 from r1 to r2
bool AliasedRegisters::has_edge_between(const Value& r1,
                                        const Value& r2) const {
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
  return edge_search != adj_end;
}

std::vector<AliasedRegisters::vertex_t> AliasedRegisters::vertices_in_group(
    vertex_t v) const {
  std::vector<vertex_t> result;
  result.push_back(v);
  const auto& adj = boost::adjacent_vertices(v, m_graph);
  const auto& adj_begin = adj.first;
  const auto& adj_end = adj.second;
  for (auto it = adj_begin; it != adj_end; ++it) {
    result.push_back(*it);
  }
  return result;
}

void AliasedRegisters::merge_groups_of(const Value& r1, const Value& r2) {
  vertex_t v1 = find_or_create(r1);
  vertex_t v2 = find_or_create(r2);

  for (vertex_t g1 : vertices_in_group(v1)) {
    for (vertex_t g2 : vertices_in_group(v2)) {
      boost::add_edge(g1, g2, m_graph);
    }
  }
}

// ---- extends AbstractValue ----

void AliasedRegisters::clear() {
  m_graph.clear();
}

AliasedRegisters::Kind AliasedRegisters::kind() const {
  return (boost::num_edges(m_graph) > 0) ? AliasedRegisters::Kind::Value
                                         : AliasedRegisters::Kind::Top;
}

// The lattice looks like this:
//
//             T (graphs with no edges)
//      graphs with 1 edge                  ^  join moves up (edge intersection)
//      graphs with 2 edges                 |
//            ...                           v  meet moves down (edge union)
//      graphs with n edges
//            ...
//            _|_
//
// So, leq is the superset relation on the edge set
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
    const Value& r1 = other.m_graph[boost::source(*it, other.m_graph)];
    const Value& r2 = other.m_graph[boost::target(*it, other.m_graph)];
    if (!has_edge_between(r1, r2)) {
      return false;
    }
  }
  return true;
}

// returns true iff they have exactly the same edges between the same Values
bool AliasedRegisters::equals(const AliasedRegisters& other) const {
  return boost::num_edges(m_graph) == boost::num_edges(other.m_graph) &&
         leq(other);
}

// alias group union
AliasedRegisters::Kind AliasedRegisters::meet_with(
    const AliasedRegisters& other) {
  const auto& iters = boost::edges(other.m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    const Value& r1 = other.m_graph[boost::source(*it, other.m_graph)];
    const Value& r2 = other.m_graph[boost::target(*it, other.m_graph)];
    if (!this->are_aliases(r1, r2)) {
      this->merge_groups_of(r1, r2);
    }
  }
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
    const Value& r1 = this->m_graph[v1];
    const Value& r2 = this->m_graph[v2];
    if (!other.has_edge_between(r1, r2)) {
      deletes.emplace_back(v1, v2);
    }
  }

  // This maintains a forest of cliques because any subset of nodes of a clique
  // is also a clique
  for (const auto& edge : deletes) {
    boost::remove_edge(edge.first, edge.second, this->m_graph);
  }

  return AliasedRegisters::Kind::Value;
}

AliasedRegisters::Kind AliasedRegisters::widen_with(
    const AliasedRegisters& other) {
  return meet_with(other);
}
}
