/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <limits>

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeMap.h>

#include "ConstantUses.h"
#include "DexClass.h"

namespace aliased_registers {

class Value {
 public:
  enum class Kind : uint8_t {
    REGISTER,
    CONST_LITERAL,
    CONST_LITERAL_UPPER,
    CONST_STRING,
    CONST_TYPE,
    STATIC_FINAL,
    STATIC_FINAL_UPPER,
    NONE,
  };

 private:
  Kind m_kind;
  constant_uses::TypeDemand m_type_demand;

  union {
    reg_t m_reg;
    int64_t m_literal;
    const DexString* m_str;
    DexType* m_type;
    DexField* m_field;
    std::nullptr_t m_dummy;
  };

  // hide these constuctors in favor of the named version below
  explicit Value(Kind k, reg_t r) {
    always_assert(k == Kind::REGISTER);
    m_kind = k;
    m_reg = r;
  }
  explicit Value(Kind k, int64_t l, constant_uses::TypeDemand td) {
    always_assert(k == Kind::CONST_LITERAL || k == Kind::CONST_LITERAL_UPPER);
    m_kind = k;
    m_literal = l;
    m_type_demand = td;
  }
  explicit Value(Kind k, DexField* f) {
    always_assert(k == Kind::STATIC_FINAL || k == Kind::STATIC_FINAL_UPPER);
    m_kind = k;
    m_field = f;
  }

 public:
  static Value create_register(reg_t r) { return Value{Kind::REGISTER, r}; }

  static Value create_literal(int64_t l, constant_uses::TypeDemand td) {
    return Value{Kind::CONST_LITERAL, l, td};
  }

  // The upper half of a wide pair
  static Value create_literal_upper(int64_t l, constant_uses::TypeDemand td) {
    return Value{Kind::CONST_LITERAL_UPPER, l, td};
  }

  static Value create_field(DexField* f) {
    return Value{Kind::STATIC_FINAL, f};
  }

  // A placeholder for the upper half of the value held by this field. When an
  // sget_wide happens, we want two separate alias groups: One for the low half,
  // one for the upper half. This makes sure that the field won't connect the
  // two alias groups because {STATIC_FINAL, f} != {STATIC_FINAL_UPPER, f}.
  static Value create_field_upper(DexField* f) {
    return Value{Kind::STATIC_FINAL_UPPER, f};
  }

  explicit Value(const DexString* s) : m_kind(Kind::CONST_STRING), m_str(s) {}
  explicit Value(DexType* t) : m_kind(Kind::CONST_TYPE), m_type(t) {}
  explicit Value(DexField* f) : m_kind(Kind::STATIC_FINAL), m_field(f) {}
  explicit Value() : m_kind(Kind::NONE), m_dummy() {}

  bool operator==(const Value& other) const;
  bool operator<(const Value& other) const;
  std::string str() const;
  size_t hash() const;

  bool operator!=(const Value& other) const { return !(*this == other); }

  static const Value& none() {
    static const Value s_none;
    return s_none;
  }

  bool is_none() const { return m_kind == Kind::NONE; }

  bool is_register() const { return m_kind == Kind::REGISTER; }

  reg_t reg() const {
    always_assert(m_kind == Kind::REGISTER);
    return m_reg;
  }
};

struct ValueHash {
  size_t operator()(const Value& value) const;
};

using vertex_t = uint32_t;

/*
 * Bidirectional mapping of values to vertex indices with fast look-ups in both
 * directions.
 */
class VertexValues {
 private:
  mutable std::vector<Value> m_values;
  mutable std::unordered_map<Value, vertex_t, ValueHash> m_indices;

 public:
  vertex_t get_vertex(const Value& value) const;
  const Value& get_value(vertex_t v) const { return m_values.at(v - 1); }
};

using VertexMapping = std::function<vertex_t(vertex_t)>;

/*
 * Bidirectional graph over Value. Provides exactly those operations needed by
 * AliasedRegisters. In particular, each vertex can have at most one out-edge.
 * The out-edge points to the representative of a group, and in-edges point to
 * all other members of a group. The graph and all copies of it will share one
 * underlying VertexValues mapping.
 */
class AliasGraph {
 private:
  static const inline std::vector<vertex_t> no_vertices;

  std::shared_ptr<VertexValues> m_values;
  std::unordered_map<vertex_t, std::vector<vertex_t>> m_vertices_ins;
  std::unordered_map<vertex_t, vertex_t> m_vertices_outs;
  size_t m_edges{0};

 public:
  AliasGraph();

  bool same_vertices(const AliasGraph& other) const {
    return m_values == other.m_values;
  }

  vertex_t get_vertex(const Value& value) const {
    always_assert(value != Value());
    return m_values->get_vertex(value);
  }

  const Value& operator[](vertex_t v) const { return m_values->get_value(v); }

  void add_edge(vertex_t u, vertex_t v);

  void remove_edge(vertex_t u, vertex_t v);

  size_t edges_count() const { return m_edges; }

  std::optional<vertex_t> adjacent_vertex(vertex_t v) const {
    auto v_it = m_vertices_outs.find(v);
    return v_it == m_vertices_outs.end()
               ? std::nullopt
               : std::optional<vertex_t>(v_it->second);
  }

  const std::vector<vertex_t>& inv_adjacent_vertices(vertex_t v) const {
    auto v_it = m_vertices_ins.find(v);
    return v_it == m_vertices_ins.end() ? no_vertices : v_it->second;
  }

  const std::unordered_map<vertex_t, vertex_t>&
  get_vertices_with_adjacent_vertex() const {
    return m_vertices_outs;
  }

  const std::unordered_map<vertex_t, std::vector<vertex_t>>&
  get_vertices_with_inv_adjacent_vertices() const {
    return m_vertices_ins;
  }

  void clear_vertex(vertex_t v);

  void clear();
};

class AliasedRegisters final : public sparta::AbstractValue<AliasedRegisters> {
 public:
  // Declare that `moving` is an alias of `group`
  // by adding `moving` into the alias group of `group`
  void move(const Value& moving, const Value& group);

  // break every alias that any register has to `r`
  void break_alias(const Value& r);

  // Are r1 and r2 aliases?
  // (including transitive aliases)
  // Use for testing only, as this is relatively expensive, scanning all aliases
  // of r1.
  bool are_aliases(const Value& r1, const Value& r2) const;

  // Each alias group has one representative register
  reg_t get_representative(
      const Value& orig,
      const boost::optional<reg_t>& max_addressable = boost::none) const;

  // ---- extends AbstractValue ----

  void clear();

  sparta::AbstractValueKind kind() const;

  bool leq(const AliasedRegisters& other) const;

  bool equals(const AliasedRegisters& other) const;

  sparta::AbstractValueKind join_with(const AliasedRegisters& other);

  sparta::AbstractValueKind widen_with(const AliasedRegisters& other);

  sparta::AbstractValueKind meet_with(const AliasedRegisters& other);

  sparta::AbstractValueKind narrow_with(const AliasedRegisters& other);

 private:
  // A directed graph where register Values are vertices
  AliasGraph m_graph;

  // For keeping track of the oldest representative.
  //
  // When adding a vertex to a group, it gets 1 + the max insertion number of
  // the group. When choosing a representative, we prefer lower insertion
  // numbers. Vertices in a group are guaranteed to have an entry in this map.
  // Do not query this map if the vertex is not in a group.
  //
  // We only track the insertion for registers because they're the only type
  // that could be chosen as a representative.
  using InsertionOrder = sparta::PatriciaTreeMap<vertex_t, uint32_t>;
  InsertionOrder m_insert_order;

  boost::optional<vertex_t> find_in_tree(const Value& r,
                                         vertex_t in_this_tree) const;
  vertex_t find_root(vertex_t v) const;
  vertex_t find_new_root(vertex_t old_root) const;

  void change_root_helper(vertex_t old_root,
                          boost::optional<vertex_t> maybe_new_root);
  void maybe_change_root(vertex_t old_root);
  void change_root_to(vertex_t old_root, vertex_t new_root);

  // return a vector of all vertices in a root's alias group (including root
  // itself)
  std::vector<vertex_t> vertices_in_group(vertex_t root) const;

  // return all groups (not including singletons)
  std::vector<std::vector<vertex_t>> all_groups();

  void track_insert_order(const Value& moving,
                          vertex_t v_moving,
                          const Value& group,
                          vertex_t v_group,
                          const std::vector<vertex_t>& grp);
  void clear_insert_number(vertex_t v);
  void handle_edge_intersection_insert_order(
      const InsertionOrder& other_insert_order,
      const VertexMapping& vertex_mapping);
  void handle_insert_order_at_merge(const std::vector<vertex_t>& group,
                                    const InsertionOrder& other_insert_order,
                                    const VertexMapping& vertex_mapping);
  void renumber_insert_order(
      std::vector<vertex_t> registers,
      const std::function<bool(vertex_t, vertex_t)>& less_than);

  bool has_incoming(vertex_t v) const;
  bool has_outgoing(vertex_t v) const;

  VertexMapping get_vertex_mapping(const AliasedRegisters& other) const;

  std::string dump() const;
};

class AliasDomain final
    : public sparta::AbstractDomainScaffolding<
          sparta::CopyOnWriteAbstractValue<AliasedRegisters>,
          AliasDomain> {
 public:
  explicit AliasDomain(
      sparta::AbstractValueKind kind = sparta::AbstractValueKind::Top)
      : AbstractDomainScaffolding(kind) {}

  void update(const std::function<void(AliasedRegisters&)>& operation) {
    if (is_bottom()) {
      return;
    }
    operation(this->get_value()->get());
    normalize();
  }
};

} // namespace aliased_registers
