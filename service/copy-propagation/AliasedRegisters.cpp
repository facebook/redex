/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include <boost/range/algorithm.hpp>
#include <boost/range/iterator_range.hpp>
#include <limits>
#include <numeric>
#include <unordered_set>

using namespace sparta;

// AliasedRegisters is a data structure that CopyPropagation uses to keep track
// of which Values (registers, constants, final fields, etc.) are the same
// (aliased).
//
// For example,
//   move v1, v0
//   move v2, v0
//   move v1, v2 ; delete this instruction because v1 and v2 are already aliased
//
// AliasedRegisters uses a graph to keep track of these alias relationships,
// where nodes are Values. The graph is a forest of trees, where each tree is an
// "alias group", meaning that all Values in the group (tree) are aliased to
// each other. Creating groups like this implements the transitive nature of the
// aliasing relationship.
//
// This is similar in concept to union/find. But it also needs to support
// deleting an element and intersecting two data structures, which is why we
// have a custom implementation.
//
// The implementation is similar to a link/cut tree, but these trees only have
// two levels (every node is either a root or a leaf). The reason for the two
// level design is because we are not actually supporting the "cut" part of the
// link/cut tree. After two groups A and B get unioned, if one of the elements
// of B gets overwritten, we only want to remove that single element from the
// group instead of splitting off all the elements that were formerly in B. So
// it's more of a link/delete tree.
//
// A single group could be represented as multiple different trees (by choosing
// different roots), but this is undesirable. To enforce canonical trees, we use
// `Value::operator<` to ensure that the minimum node is always the root node.
// This incurs the cost of changing the root node more frequently, but it's
// worth it over all, especially when computing the intersection of two graphs.
//
// The aliasing relation is an equivalence relation. An alias group is an
// equivalence class of this relation.
//   Reflexive : A node is trivially equivalent to itself
//   Symmetric : If two nodes have the same root, they must be in the same tree
//   Transitive: `AliasedRegisters::move` adds an edge from the new node the
//               root of the tree

namespace aliased_registers {

using vertex_t = AliasedRegisters::vertex_t;

// Move `moving` into the alias group of `group`
void AliasedRegisters::move(const Value& moving, const Value& group) {
  always_assert_log(!moving.is_none() && !group.is_none(),
                    "Neither should be NONE. %s, %s",
                    moving.str().c_str(),
                    group.str().c_str());
  // Only need to do something if they're not already in same group
  if (!are_aliases(moving, group)) {
    // remove from the old group
    break_alias(moving);
    vertex_t v_moving = find_or_create(moving);
    vertex_t v_group = find_or_create(group);

    const auto& grp = vertices_in_group(v_group);
    track_insert_order(moving, v_moving, group, v_group, grp);

    // Add an edge from `moving` to the root of its new group
    // This maintains a maximum of 2 levels in the tree.
    // Therefore, root nodes are the only nodes with incoming edges.
    vertex_t v_group_root = find_root(v_group);
    boost::add_edge(v_moving, v_group_root, m_graph);

    // We want to have a single canonical representation of a tree. Make sure
    // the root is always the node that sorts lowest of the Values in this tree
    const Value& group_root = m_graph[v_group_root];
    if (moving < group_root) {
      change_root_to(v_group_root, v_moving);
    }
  }
}

// Set the insertion number of `v_moving` to 1 + the max of `group`.
// `v_moving` is the newest member of `group` so it should have the highest
// insertion number.
//
// If this call creates a new group (of size two), also set the insertion
// number of `v_group`
void AliasedRegisters::track_insert_order(const Value& moving,
                                          vertex_t v_moving,
                                          const Value& group,
                                          vertex_t v_group,
                                          const std::vector<vertex_t>& grp) {
  always_assert(!grp.empty());
  if (grp.size() == 1 && group.is_register()) {
    // We're creating a new group from a singleton. The `group`
    // register is the oldest, followed by `moving`.
    m_insert_order.insert_or_assign(v_group, 0);
  }

  if (moving.is_register()) {
    uint32_t moving_index =
        1U + std::accumulate(
                 grp.begin(), grp.end(), 0U, [this](uint32_t acc, vertex_t v) {
                   // use operator[] to ignore non-register group members
                   return std::max(acc, m_insert_order.at(v));
                 });
    m_insert_order.insert_or_assign(v_moving, moving_index);
  }
}

// Remove `r` from its alias group
void AliasedRegisters::break_alias(const Value& r) {
  const auto& v = find(r);
  if (v) {
    // if `v` was the root of a tree, we need to promote a leaf
    maybe_change_root(*v);

    // clear removes all edges to and from `r`
    boost::clear_vertex(*v, m_graph);

    if (r.is_register()) {
      // `v` is not in a group any more so it has no insert order
      clear_insert_number(*v);
    }
  }
}

// Call this when `v` should no longer have an insertion number (because it does
// not belong to a group).
void AliasedRegisters::clear_insert_number(vertex_t v) {
  m_insert_order.remove(v);
}

// Two Values are aliased when they are in the same tree
bool AliasedRegisters::are_aliases(const Value& r1, const Value& r2) const {
  if (r1 == r2) {
    return true;
  }
  const auto& v1 = find(r1);
  return v1 != boost::none && find_in_tree(r2, *v1) != boost::none;
}

// Two vertices are aliased when they have the same root
bool AliasedRegisters::vertices_are_aliases(vertex_t v1, vertex_t v2) const {
  return v1 == v2 || find_root(v1) == find_root(v2);
}

// Return the vertex of the root node of the tree that `v` belongs to
// If `v` is not part of a group, then it is a singleton and it is its own root
// node.
vertex_t AliasedRegisters::find_root(vertex_t v) const {
  // The trees only have two levels. No need to loop
  const auto& adj = boost::adjacent_vertices(v, m_graph);
  const auto& begin = adj.first;
  const auto& end = adj.second;
  if (begin == end) {
    // `v` is its own root
    return v;
  }
  always_assert_log(std::next(begin) == end,
                    "a tree can't have more than one root");
  return *begin;
}

// If `old_root` is a root node, promote a different node from this tree to the
// root.
void AliasedRegisters::change_root_helper(
    vertex_t old_root, boost::optional<vertex_t> maybe_new_root) {
  const auto& in_adj = boost::inv_adjacent_vertices(old_root, m_graph);
  const auto& in_begin = in_adj.first;
  const auto& in_end = in_adj.second;
  if (in_begin != in_end) {
    always_assert_log(
        !has_outgoing(old_root), "Only 2 levels allowed\n%s", dump().c_str());
    vertex_t new_root = (maybe_new_root == boost::none)
                            ? find_new_root(old_root)
                            : *maybe_new_root;
    if (new_root != old_root) {
      always_assert_log(
          !has_incoming(new_root), "Only 2 levels allowed\n%s", dump().c_str());
      std::vector<vertex_t> leaves;
      leaves.reserve((in_end - in_begin) - 1);
      for (auto it = in_begin; it != in_end; ++it) {
        if (*it != new_root) {
          leaves.push_back(*it);
        }
      }
      // For all nodes in the tree that aren't the new or old root,
      // redirect their outgoing edges to the new root
      for (vertex_t leaf : leaves) {
        boost::remove_edge(leaf, old_root, m_graph);
        boost::add_edge(leaf, new_root, m_graph);
      }
      // reverse the edge between the old root and the new root
      boost::remove_edge(new_root, old_root, m_graph);
      boost::add_edge(old_root, new_root, m_graph);
    }
  }
}

// If `old_root` is a root, promote one of its leaves to the root.
// Otherwise, do nothing.
void AliasedRegisters::maybe_change_root(vertex_t old_root) {
  change_root_helper(old_root, boost::none);
}

// Promote `new_root` to a root and demote `old_root` to a leaf
void AliasedRegisters::change_root_to(vertex_t old_root, vertex_t new_root) {
  always_assert(old_root != new_root);
  always_assert(has_incoming(old_root));
  change_root_helper(old_root, new_root);
}

// We want to have a single canonical representation of a tree. the new root is
// the node that sorts lowest of the leaves in this tree
vertex_t AliasedRegisters::find_new_root(vertex_t old_root) const {
  const auto& in_adj = boost::inv_adjacent_vertices(old_root, m_graph);
  const auto& in_begin = in_adj.first;
  const auto& in_end = in_adj.second;
  always_assert_log(in_begin != in_end, "%s", dump().c_str());
  return *boost::range::min_element(in_adj, [this](vertex_t v1, vertex_t v2) {
    return m_graph[v1] < m_graph[v2];
  });
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
reg_t AliasedRegisters::get_representative(
    const Value& orig, const boost::optional<reg_t>& max_addressable) const {
  always_assert(orig.is_register());

  // if orig is not in the graph, then it has no representative
  const auto& v = find(orig);
  if (!v) {
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
// if not, return boost::none.
// WARNING: this operation is expensive on large graphs. It's an O(n) search of
// all the nodes.
boost::optional<vertex_t> AliasedRegisters::find(const Value& r) const {
  const auto& iters = boost::vertices(m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    if (m_graph[*it] == r) {
      return *it;
    }
  }
  return boost::none;
}

// If any nodes in the same tree as `in_this_tree` have the Value `r`, then
// return `r`'s vertex.
// This is a faster alternative to a `find()` over the entire graph
boost::optional<vertex_t> AliasedRegisters::find_in_tree(
    const Value& r, vertex_t in_this_tree) const {
  vertex_t root = find_root(in_this_tree);
  if (m_graph[root] == r) {
    return root;
  }
  const auto& adj = boost::inv_adjacent_vertices(root, m_graph);
  const auto& begin = adj.first;
  const auto& end = adj.second;
  for (auto it = begin; it != end; ++it) {
    if (m_graph[*it] == r) {
      return *it;
    }
  }
  return boost::none;
}

// returns the vertex holding `r` or creates a new (unconnected)
// vertex if `r` is not in m_graph
vertex_t AliasedRegisters::find_or_create(const Value& r) {
  const auto& it = find(r);
  if (it) {
    return *it;
  } else {
    vertex_t v = boost::add_vertex(r, m_graph);
    return v;
  }
}

// return a vector of `v` and all the vertices in the same tree
// (with the root first)
std::vector<vertex_t> AliasedRegisters::vertices_in_group(vertex_t v) const {
  vertex_t root = find_root(v);
  const auto& in_adj = boost::inv_adjacent_vertices(root, m_graph);
  const auto& in_begin = in_adj.first;
  const auto& in_end = in_adj.second;
  std::vector<vertex_t> result;
  result.reserve(1 + (in_end - in_begin));
  result.push_back(root);
  for (auto it = in_begin; it != in_end; ++it) {
    result.push_back(*it);
  }
  return result;
}

// return true if `v` has no neighbors
bool AliasedRegisters::is_singleton(vertex_t v) {
  return !has_outgoing(v) && !has_incoming(v);
}

// return true if `v` has any incoming edges (equivalent to v being a root of a
// non-singleton tree)
bool AliasedRegisters::has_incoming(vertex_t v) {
  const auto& in_adj = boost::inv_adjacent_vertices(v, m_graph);
  const auto& in_begin = in_adj.first;
  const auto& in_end = in_adj.second;
  return in_begin != in_end;
}

// return true if `v` has any outgoing edges (equivalent to v being a leaf)
bool AliasedRegisters::has_outgoing(vertex_t v) {
  const auto& out_adj = boost::adjacent_vertices(v, m_graph);
  const auto& out_begin = out_adj.first;
  const auto& out_end = out_adj.second;
  return out_begin != out_end;
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

// leq (<=) is the superset relation on the alias groups
bool AliasedRegisters::leq(const AliasedRegisters& other) const {
  if (boost::num_edges(m_graph) < boost::num_edges(other.m_graph)) {
    // this cannot be a superset of other if this has fewer edges
    return false;
  }

  // for all edges in `other` (the potential subset), make sure `this` has that
  // alias relationship
  const auto& iters = boost::edges(other.m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    const Value& r1 = other.m_graph[boost::source(*it, other.m_graph)];
    const Value& r2 = other.m_graph[boost::target(*it, other.m_graph)];
    if (!are_aliases(r1, r2)) {
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
  not_reached_log("UNUSED");
}

// Alias group intersection.
// Only keep the alias relationships that both `this` and `other` contain.
AbstractValueKind AliasedRegisters::join_with(const AliasedRegisters& other) {
  auto this_before_groups = this->all_groups();

  // Remove all edges from this graph. We will add back the ones that `other`
  // also has
  using edge_t = boost::graph_traits<Graph>::edge_descriptor;
  boost::remove_edge_if([](const edge_t&) { return true; }, m_graph);

  // Break up each group into some number of new groups.
  // Intersection can't create any groups larger than what `this` had, only the
  // same size or smaller.
  for (auto& group : this_before_groups) {
    // Sort so that we only have to scan the nodes after the potential root
    // (remember that the root node must sort lowest of its tree)
    std::sort(group.begin(), group.end(), [this](vertex_t v1, vertex_t v2) {
      return m_graph[v1] < m_graph[v2];
    });
    // Starting with the lowest node, grab all the singletons that `other`
    // agrees are aliased with this potential root.
    // Repeat with the next potential root.
    size_t sz = group.size();
    for (size_t i = 0; i < sz; ++i) {
      vertex_t new_root = group[i];
      if (has_outgoing(new_root)) {
        // This can't be a new root because it's already a leaf
        continue;
      }
      // We can start iterating from i + 1 because everything beneath `i` has
      // already been considered as a potential root and had all possible nodes
      // added to its tree.
      for (size_t j = i + 1; j < sz; ++j) {
        // If `v` hasn't already been added to another tree and `other` agrees,
        // then add the edge.
        vertex_t v = group[j];
        if (!has_outgoing(v) &&
            other.are_aliases(this->m_graph[v], this->m_graph[new_root])) {
          boost::add_edge(v, new_root, m_graph);
        }
      }
    }
  }

  handle_edge_intersection_insert_order(other);
  return AbstractValueKind::Value;
}

void AliasedRegisters::handle_edge_intersection_insert_order(
    const AliasedRegisters& other) {
  // Clear out stale values in `m_insert_order` for vertices removed from
  // groups.
  const auto& iters = boost::vertices(this->m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    if (is_singleton(*it)) {
      clear_insert_number(*it);
    }
  }

  // Assign new insertion numbers while taking into account both insertion maps.
  for (auto& group : all_groups()) {
    handle_insert_order_at_merge(group, other);
  }
}

// Merge the ordering in `other.m_insert_order` into `this->m_insert_order`.
//
// Note that by construction of m_insert_order, it gives us a total order of all
// elements in the merged groups, in this and the other aliased-registers.
// The new order follows the sum of the individual pointwise orders, using
// register numbers as tie breakers.
void AliasedRegisters::handle_insert_order_at_merge(
    const std::vector<vertex_t>& group, const AliasedRegisters& other) {
  std::unordered_map<vertex_t, uint32_t> insert_order_sums;
  std::vector<vertex_t> registers;
  always_assert(!group.empty());
  auto other_front = *other.find(m_graph[group.front()]);
  for (auto v : group) {
    const Value& value = this->m_graph[v];
    if (!value.is_register()) {
      continue;
    }
    auto this_i = m_insert_order.at(v);
    auto other_v = *other.find_in_tree(m_graph[v], other_front);
    auto other_i = other.m_insert_order.at(other_v);
    auto emplaced = insert_order_sums.emplace(v, this_i + other_i).second;
    always_assert(emplaced);
    registers.push_back(v);
  }

  auto less_than = [this, &insert_order_sums](vertex_t a, vertex_t b) {
    // return true if `a` occurs before `b`.
    // return false if they compare equal or if `b` occurs
    // before `a`.

    if (a == b) return false;

    auto i_sum = insert_order_sums.at(a);
    auto j_sum = insert_order_sums.at(b);

    if (i_sum != j_sum) {
      return i_sum < j_sum;
    }

    // Tie-breaker: Register numbers.
    const Value& val_a = this->m_graph[a];
    const Value& val_b = this->m_graph[b];
    return val_a.reg() < val_b.reg();
  };
  renumber_insert_order(std::move(registers), less_than);
}

// Rewrite the insertion number of all registers in `group` in an order defined
// by `less_than`
void AliasedRegisters::renumber_insert_order(
    std::vector<vertex_t> registers,
    const std::function<bool(vertex_t, vertex_t)>& less_than) {

  if (registers.size() < 2) {
    // No need to assign insert order for singletons
    return;
  }

  // Assign new insertion numbers based on sorting.
  std::sort(registers.begin(), registers.end(), less_than);
  uint32_t i = 0;
  for (vertex_t v : registers) {
    this->m_insert_order.insert_or_assign(v, i);
    ++i;
  }
}

// return all groups (not including singletons)
std::vector<std::vector<vertex_t>> AliasedRegisters::all_groups() {
  std::vector<std::vector<vertex_t>> result;
  std::unordered_set<vertex_t> visited;

  const auto& iters = boost::vertices(this->m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  for (auto it = begin; it != end; ++it) {
    vertex_t root = find_root(*it);
    const auto& pair = visited.insert(root);
    bool insertion_took_place = pair.second;
    if (insertion_took_place) {
      auto group = vertices_in_group(root);
      if (group.size() > 1) {
        result.emplace_back(std::move(group));
      }
    }
  }
  return result;
}

// returns a string representation of this data structure. Intended for
// debugging.
std::string AliasedRegisters::dump() const {
  std::ostringstream oss;
  const auto& iters = boost::edges(this->m_graph);
  const auto& begin = iters.first;
  const auto& end = iters.second;
  oss << "Graph [" << std::endl;
  for (auto it = begin; it != end; ++it) {
    const Value& r1 = m_graph[boost::source(*it, m_graph)];
    const Value& r2 = m_graph[boost::target(*it, m_graph)];
    oss << "(" << r1.str().c_str() << " -> " << r2.str().c_str() << ") "
        << std::endl;
  }
  oss << "] insert order [" << std::endl;
  for (const auto& entry : m_insert_order) {
    vertex_t v = entry.first;
    const Value& r = m_graph[v];
    uint32_t i = entry.second;
    oss << r.str().c_str() << " has index " << i << std::endl;
  }
  oss << "]" << std::endl;
  return oss.str();
}

bool Value::operator==(const Value& other) const {
  if (m_kind != other.m_kind) {
    return false;
  }

  switch (m_kind) {
  case Kind::REGISTER:
    return m_reg == other.m_reg;
  case Kind::CONST_LITERAL:
  case Kind::CONST_LITERAL_UPPER:
    return m_literal == other.m_literal && m_type_demand == other.m_type_demand;
  case Kind::CONST_STRING:
    return m_str == other.m_str;
  case Kind::CONST_TYPE:
    return m_type == other.m_type;
  case Kind::STATIC_FINAL:
  case Kind::STATIC_FINAL_UPPER:
    return m_field == other.m_field;
  case Kind::NONE:
    return true;
  }
}

bool Value::operator<(const Value& other) const {
  if (m_kind != other.m_kind) {
    return m_kind < other.m_kind;
  }
  switch (m_kind) {
  case Kind::REGISTER:
    return m_reg < other.m_reg;
  case Kind::CONST_LITERAL:
  case Kind::CONST_LITERAL_UPPER:
    if (m_literal != other.m_literal) {
      return m_literal < other.m_literal;
    } else {
      return m_type_demand < other.m_type_demand;
    }
  case Kind::CONST_STRING:
    return compare_dexstrings(m_str, other.m_str);
  case Kind::CONST_TYPE:
    return compare_dextypes(m_type, other.m_type);
  case Kind::STATIC_FINAL:
  case Kind::STATIC_FINAL_UPPER:
    return compare_dexfields(m_field, other.m_field);
  case Kind::NONE:
    not_reached_log("can't sort NONEs");
  }
}

// returns a string representation of this Value. Intended for debugging.
std::string Value::str() const {
  std::ostringstream oss;
  switch (m_kind) {
  case Kind::REGISTER:
    oss << "v" << m_reg;
    break;
  case Kind::CONST_LITERAL:
    oss << m_literal;
    break;
  case Kind::CONST_LITERAL_UPPER:
    oss << m_literal << " upper";
    break;
  case Kind::CONST_STRING:
    oss << m_str->str();
    break;
  case Kind::CONST_TYPE:
    oss << m_type->str();
    break;
  case Kind::STATIC_FINAL:
    oss << m_field->str();
    break;
  case Kind::STATIC_FINAL_UPPER:
    oss << m_field->str() << " upper";
    break;
  case Kind::NONE:
    oss << "NONE";
    break;
  }
  return oss.str();
}

} // namespace aliased_registers
