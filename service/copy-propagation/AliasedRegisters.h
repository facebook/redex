/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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

#include "AbstractDomain.h"
#include "ConstantUses.h"
#include "DexClass.h"

namespace aliased_registers {

using Register = uint32_t;
const Register RESULT_REGISTER = std::numeric_limits<Register>::max() - 1;

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
    Register m_reg;
    int64_t m_literal;
    DexString* m_str;
    DexType* m_type;
    DexField* m_field;
    std::nullptr_t m_dummy;
  };

  // hide these constuctors in favor of the named version below
  explicit Value(Kind k, Register r) {
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
  static Value create_register(Register r) { return Value{Kind::REGISTER, r}; }

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

  explicit Value(DexString* s) : m_kind(Kind::CONST_STRING), m_str(s) {}
  explicit Value(DexType* t) : m_kind(Kind::CONST_TYPE), m_type(t) {}
  explicit Value(DexField* f) : m_kind(Kind::STATIC_FINAL), m_field(f) {}
  explicit Value() : m_kind(Kind::NONE), m_dummy() {}

  bool operator==(const Value& other) const {
    if (m_kind != other.m_kind) {
      return false;
    }

    switch (m_kind) {
    case Kind::REGISTER:
      return m_reg == other.m_reg;
    case Kind::CONST_LITERAL:
    case Kind::CONST_LITERAL_UPPER:
      return m_literal == other.m_literal &&
             m_type_demand == other.m_type_demand;
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

  bool operator!=(const Value& other) const { return !(*this == other); }

  static const Value& none() {
    static const Value s_none;
    return s_none;
  }

  bool is_none() const { return m_kind == Kind::NONE; }

  bool is_register() const { return m_kind == Kind::REGISTER; }

  Register reg() const {
    always_assert(m_kind == Kind::REGISTER);
    return m_reg;
  }
};

class AliasedRegisters final : public sparta::AbstractValue<AliasedRegisters> {
 public:
  AliasedRegisters() {}

  // Declare that `moving` is an alias of `group`
  // by adding `moving` into the alias group of `group`
  void move(const Value& moving, const Value& group);

  // break every alias that any register has to `r`
  void break_alias(const Value& r);

  // Are r1 and r2 aliases?
  // (including transitive aliases)
  bool are_aliases(const Value& r1, const Value& r2) const;

  // Each alias group has one representative register
  Register get_representative(
      const Value& r,
      const boost::optional<Register>& max_addressable = boost::none) const;

  // ---- extends AbstractValue ----

  void clear() override;

  sparta::AbstractValueKind kind() const override;

  bool leq(const AliasedRegisters& other) const override;

  bool equals(const AliasedRegisters& other) const override;

  sparta::AbstractValueKind join_with(const AliasedRegisters& other) override;

  sparta::AbstractValueKind widen_with(const AliasedRegisters& other) override;

  sparta::AbstractValueKind meet_with(const AliasedRegisters& other) override;

  sparta::AbstractValueKind narrow_with(const AliasedRegisters& other) override;

 private:
  // An undirected graph where register values are vertices
  // and an edge means they are aliased.
  // Using a set for the edge container makes sure we can't have parallel edges
  using Graph = boost::adjacency_list<boost::setS, // out edge container
                                      boost::vecS, // vertex container
                                      boost::undirectedS, // undirected graph
                                      Value>; // node property
  using vertex_t = boost::graph_traits<Graph>::vertex_descriptor;
  Graph m_graph;

  // For keeping track of the oldest representative.
  //
  // When adding a vertex to a group, it gets 1 + the max insertion number of
  // the group. When choosing a representative, we prefer lower insertion
  // numbers. Vertices in a group are guaranteed to have an entry in this map.
  // Do not query this map if the vertex is not in a group.
  //
  // We only track the insertion for registers because they're the only type
  // that could be chosen as a representative.
  using InsertionOrder = std::unordered_map<vertex_t, size_t>;
  InsertionOrder m_insert_order;

  const boost::range_detail::integer_iterator<vertex_t> find(
      const Value& r) const;

  vertex_t find_or_create(const Value& r);

  bool has_edge_between(const Value& r1, const Value& r2) const;
  bool are_adjacent(vertex_t v1, vertex_t v2) const;

  // return a vector of all vertices in v's alias group (including v itself)
  std::vector<vertex_t> vertices_in_group(vertex_t v) const;

  // merge r1's group with r2. This operation is symmetric
  void merge_groups_of(const Value& r1,
                       const Value& r2,
                       const AliasedRegisters& other);

  // return all groups (not including singletons)
  std::vector<std::vector<vertex_t>> all_groups();

  void track_insert_order(const Value& moving,
                          vertex_t v_moving,
                          const Value& group,
                          vertex_t v_group,
                          const std::vector<vertex_t>& grp);
  void clear_insert_number(vertex_t v);
  void handle_edge_intersection_insert_order(const AliasedRegisters& other);
  void handle_insert_order_at_merge(const std::vector<vertex_t>& group,
                                    const AliasedRegisters& other);

  void renumber_insert_order(
      std::vector<vertex_t> group,
      const std::function<bool(vertex_t, vertex_t)>& less_than);

  // return true if v has any neighboring vertices
  bool has_neighbors(vertex_t v);
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
