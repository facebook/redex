/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
  reg_t get_representative(
      const Value& orig,
      const boost::optional<reg_t>& max_addressable = boost::none) const;

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
  // A directed graph where register Values are vertices
  using Graph =
      boost::adjacency_list<boost::vecS, // out edge container
                            boost::vecS, // vertex container
                            boost::bidirectionalS, // directed graph with access
                                                   // to both incoming and
                                                   // outgoing edges
                            Value>; // node property
  Graph m_graph;

 public:
  using vertex_t = boost::graph_traits<Graph>::vertex_descriptor;

 private:
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

  boost::optional<vertex_t> find(const Value& r) const;
  boost::optional<vertex_t> find_in_tree(const Value& r,
                                         vertex_t in_this_tree) const;
  vertex_t find_or_create(const Value& r);
  vertex_t find_root(vertex_t v) const;
  vertex_t find_new_root(vertex_t old_root) const;

  void change_root_helper(vertex_t old_root,
                          boost::optional<vertex_t> maybe_new_root);
  void maybe_change_root(vertex_t old_root);
  void change_root_to(vertex_t old_root, vertex_t new_root);

  bool has_edge_between(const Value& r1, const Value& r2) const;
  bool vertices_are_aliases(vertex_t v1, vertex_t v2) const;

  // return a vector of all vertices in v's alias group (including v itself)
  std::vector<vertex_t> vertices_in_group(vertex_t v) const;

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

  bool is_singleton(vertex_t v);
  bool has_incoming(vertex_t v);
  bool has_outgoing(vertex_t v);

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
