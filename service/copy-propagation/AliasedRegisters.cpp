/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
#include <numeric>
#include <unordered_set>

using namespace sparta;

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
// added an edge from v0 to v1, then we would have lost this information.
void AliasedRegisters::move(const Value& moving, const Value& group) {
  // Only need to do something if they're not already in same group
  if (!are_aliases(moving, group)) {
    // remove from the old group
    break_alias(moving);
    vertex_t v_moving = find_or_create(moving);
    vertex_t v_group = find_or_create(group);

    const auto& grp = vertices_in_group(v_group);
    track_insert_order(moving, v_moving, group, v_group, grp);

    // add edge to every node in new group
    for (vertex_t v : grp) {
      boost::add_edge(v_moving, v, m_graph);
    }
  }
}

// Set the insertion number of `v_moving` to 1 + the max of `group`.
// `v_moving` is the newest member of `group` so it should have the highest
// insertion number.
//
// If this call creates a new clique (of size two), also set the insertion
// number of v_group
void AliasedRegisters::track_insert_order(const Value& moving,
                                          vertex_t v_moving,
                                          const Value& group,
                                          vertex_t v_group,
                                          const std::vector<vertex_t>& grp) {
  always_assert(!grp.empty());
  if (grp.size() == 1 && group.is_register()) {
    // We're creating a new clique from a solitary vertex. The group
    // register is the oldest, followed by moving.
    m_insert_order[v_group] = 0;
  }

  if (moving.is_register()) {
    m_insert_order[v_moving] =
        1 + std::accumulate(
                grp.begin(), grp.end(), 0, [this](size_t acc, vertex_t v) {
                  // use operator[] to ignore non-register group members
                  return std::max(acc, m_insert_order[v]);
                });
  }
}

// Remove r from its alias group
void AliasedRegisters::break_alias(const Value& r) {
  const auto& v = find(r);
  const auto& end = boost::vertices(m_graph).second;
  if (v != end) {
    // clear removes all edges incident to r
    boost::clear_vertex(*v, m_graph);

    if (r.is_register()) {
      // v is not in a clique any more so it has no insert order
      clear_insert_number(*v);
    }
  }
}

// Call this when `v` should no longer have an insertion number (because it does
// not belong to a clique).
void AliasedRegisters::clear_insert_number(vertex_t v) {
  m_insert_order.erase(v);
}

// if there is an edge between r1 to r2, then they are aliases.
// We only need to check for single edge paths because `move` adds an edge to
// every node in the alias group, thus maintaining transitive closure of the
// graph
bool AliasedRegisters::are_aliases(const Value& r1, const Value& r2) const {
  if (r1 == r2) {
    return true;
  }

  return has_edge_between(r1, r2);
}

// Return a representative for this register.
//
// Return the oldest register that is <= `max_addressable`
// We want the oldest register because it helps create more dead stores.
// Consider this example:
//
//   move v1, v2
//   move v0, v1
//   ; v1 is never used again
//
// if we choose v2 as our representative (not v1) then we can remove an insn:
//
//   move v0, v2
//
// `max_addressable` is useful for instructions that can only address up to v15
Register AliasedRegisters::get_representative(
    const Value& orig, const boost::optional<Register>& max_addressable) const {
  always_assert(orig.is_register());

  // if r is not in the graph, then it has no representative
  const auto& v = find(orig);
  const auto& end = boost::vertices(m_graph).second;
  if (v == end) {
    return orig.reg();
  }

  // intentionally copy the vector so we can safely remove from it
  std::vector<vertex_t> group = vertices_in_group(*v);
  // filter out non registers and other ineligible registers
  group.erase(std::remove_if(group.begin(),
                             group.end(),
                             [this, &max_addressable](vertex_t elem) {
                               // return true to remove it
                               const Value& val = m_graph[elem];
                               if (!val.is_register()) {
                                 return true;
                               }
                               return max_addressable &&
                                      val.reg() > *max_addressable;
                             }),
              group.end());

  if (group.empty()) {
    return orig.reg();
  }

  // We want the oldest element. It has the lowest insertion number
  const Value& representative = m_graph[*std::min_element(
      group.begin(), group.end(), [this](vertex_t a, vertex_t b) {
        return m_insert_order.at(a) < m_insert_order.at(b);
      })];
  return representative.reg();
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
  // find the vertex for r1
  const auto& search1 = find(r1);
  const auto& end = boost::vertices(m_graph).second;
  if (search1 == end) {
    return false;
  }
  vertex_t v1 = *search1;

  // search the neighbors of v1 for a vertex with value r2.
  const auto& adj = boost::adjacent_vertices(v1, m_graph);
  const auto& adj_begin = adj.first;
  const auto& adj_end = adj.second;
  for (auto it = adj_begin; it != adj_end; ++it) {
    if (m_graph[*it] == r2) {
      return true;
    }
  }
  return false;
}

bool AliasedRegisters::are_adjacent(vertex_t v1, vertex_t v2) const {
  // and check that they have an edge between them
  const auto& adj = boost::adjacent_vertices(v1, m_graph);
  const auto& adj_begin = adj.first;
  const auto& adj_end = adj.second;
  const auto& edge_search = std::find(adj_begin, adj_end, v2);
  return edge_search != adj_end;
}

// return a vector of v and all of it's neighbors (in no particular order)
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

// return true if v has any neighboring vertices
bool AliasedRegisters::has_neighbors(vertex_t v) {
  const auto& adj = boost::adjacent_vertices(v, m_graph);
  const auto& begin = adj.first;
  const auto& end = adj.second;
  return begin != end;
}

// ---- extends AbstractValue ----

void AliasedRegisters::clear() {
  m_graph.clear();
  m_insert_order.clear();
}

AbstractValueKind AliasedRegisters::kind() const {
  return (boost::num_edges(m_graph) > 0) ? AbstractValueKind::Value
                                         : AbstractValueKind::Top;
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

AbstractValueKind AliasedRegisters::narrow_with(const AliasedRegisters& other) {
  return meet_with(other);
}

AbstractValueKind AliasedRegisters::widen_with(const AliasedRegisters& other) {
  return join_with(other);
}

// alias group union
AbstractValueKind AliasedRegisters::meet_with(const AliasedRegisters& other) {

  const auto& iters = boost::edges(other.m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    const Value& r1 = other.m_graph[boost::source(*it, other.m_graph)];
    const Value& r2 = other.m_graph[boost::target(*it, other.m_graph)];
    if (!this->are_aliases(r1, r2)) {
      this->merge_groups_of(r1, r2, other);
    }
  }
  return AbstractValueKind::Value;
}

void AliasedRegisters::merge_groups_of(const Value& r1,
                                       const Value& r2,
                                       const AliasedRegisters& other) {
  vertex_t v1 = find_or_create(r1);
  vertex_t v2 = find_or_create(r2);

  const auto& group1 = vertices_in_group(v1);
  const auto& group2 = vertices_in_group(v2);
  std::vector<vertex_t> union_group;
  union_group.reserve(group1.size() + group2.size());
  union_group.insert(union_group.begin(), group1.begin(), group1.end());
  union_group.insert(union_group.begin(), group2.begin(), group2.end());

  handle_insert_order_at_merge(union_group, other);
  for (vertex_t g1 : group1) {
    for (vertex_t g2 : group2) {
      boost::add_edge(g1, g2, m_graph);
    }
  }
}

// edge intersection
AbstractValueKind AliasedRegisters::join_with(const AliasedRegisters& other) {

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

  handle_edge_intersection_insert_order(other);
  return AbstractValueKind::Value;
}

void AliasedRegisters::handle_edge_intersection_insert_order(
    const AliasedRegisters& other) {
  // Clear out stale values in m_insert_order for vertices removed from cliques.
  const auto& iters = boost::vertices(this->m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    if (!has_neighbors(*it)) {
      clear_insert_number(*it);
    }
  }

  // Assign new insertion numbers while taking into account both insertion maps.
  for (auto& group : all_groups()) {
    handle_insert_order_at_merge(group, other);
  }
}

// Merge the ordering in other.m_insert_order into this->m_insert_order.
//
// When both graphs know about an edge (and they don't agree about insertion
// order), use register number.
// When only one graph knows about an edge, use insertion order from that graph.
// When neither graph knows about the edge, use register number.
// This function can be used (carefully) for both union and intersection.
void AliasedRegisters::handle_insert_order_at_merge(
    const std::vector<vertex_t>& group, const AliasedRegisters& other) {
  renumber_insert_order(group, [this, &other](vertex_t a, vertex_t b) {
    // return true if a occurs before b.
    // return false if they compare equal or if b occurs before a.

    // `a` and `b` only index into this, not other.
    if (a == b) return false;
    const Value& val_a = this->m_graph[a];
    const Value& val_b = this->m_graph[b];
    auto other_a = other.find(val_a);
    auto other_b = other.find(val_b);

    bool this_has_edge = this->are_adjacent(a, b);
    bool other_has_edge = other.are_aliases(val_a, val_b);

    if (this_has_edge && other_has_edge) {
      // Intersection case should always come here
      bool this_less_than =
          this->m_insert_order.at(a) < this->m_insert_order.at(b);

      bool other_less_than =
          other.m_insert_order.at(*other_a) < other.m_insert_order.at(*other_b);

      if (this_less_than == other_less_than) {
        // The graphs agree on the order of these two vertices.
        // Preserve that order.
        return this_less_than;
      } else {
        // The graphs do not agree. Choose a deterministic order
        return val_a.reg() < val_b.reg();
      }
    } else if (this_has_edge) {
      return this->m_insert_order.at(a) < this->m_insert_order.at(b);
    } else if (other_has_edge) {
      return other.m_insert_order.at(*other_a) <
             other.m_insert_order.at(*other_b);
    } else {
      return val_a.reg() < val_b.reg();
    }
  });
}

// Rewrite the insertion number of all registers in `group` in an order defined
// by less_than
void AliasedRegisters::renumber_insert_order(
    std::vector<vertex_t> group,
    const std::function<bool(vertex_t, vertex_t)>& less_than) {

  // Filter out non registers.
  group.erase(
      std::remove_if(group.begin(),
                     group.end(),
                     [this](vertex_t v) { return !m_graph[v].is_register(); }),
      group.end());

  // Assign new insertion numbers based on sorting.
  std::sort(group.begin(), group.end(), less_than);
  size_t i = 0;
  for (vertex_t v : group) {
    this->m_insert_order[v] = i;
    ++i;
  }
}

// return all groups (not including singletons)
std::vector<std::vector<AliasedRegisters::vertex_t>>
AliasedRegisters::all_groups() {
  std::vector<std::vector<vertex_t>> result;
  std::unordered_set<vertex_t> visited;

  const auto& iters = boost::vertices(this->m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    if (visited.count(*it) == 0) {
      auto group = vertices_in_group(*it);
      for (vertex_t v : group) {
        visited.insert(v);
      }
      if (group.size() > 1) {
        result.emplace_back(std::move(group));
      }
    }
  }
  return result;
}
} // namespace aliased_registers
