/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <utility>

#include <sparta/AbstractDomain.h>
#include <sparta/HashMap.h>

namespace sparta {

namespace hae_impl {

template <typename Variable,
          typename Domain,
          typename VariableHash,
          typename VariableEqual>
class MapValue;

class value_is_bottom {};

} // namespace hae_impl

/*
 * An abstract environment is a type of abstract domain that maps the variables
 * of a program to elements of a common abstract domain. For example, to perform
 * range analysis one can use an abstract environment that maps variable names
 * to intervals:
 *
 *   {"x" -> [-1, 1], "i" -> [0, 10], ...}
 *
 * Another example is descriptive type analysis for Dex code, where one computes
 * the set of all possible Java classes a register can hold a reference to at
 * any point in the code:
 *
 *  {"v0" -> {android.app.Fragment, java.lang.Object}, "v1" -> {...}, ...}
 *
 * This type of domain is commonly used for nonrelational (also called
 * attribute-independent) analyses that do not track relationships among
 * program variables. Please note that by definition of an abstract
 * environment, if the value _|_ appears in a variable binding, then no valid
 * execution state can ever be represented by this abstract environment. Hence,
 * assigning _|_ to a variable is equivalent to setting the entire environment
 * to _|_.
 *
 * This implementation of abstract environments is based on hashtables and is
 * well suited for intraprocedural analysis. It is not intended to handle very
 * large variable sets in the thousands. We use the AbstractDomainScaffolding
 * template to build the domain. In order to minimize the size of the underlying
 * hashtable, we do not explicitly represent bindings of a variable to the Top
 * element. Hence, any variable that is not explicitly represented in the
 * environment has a default value of Top. This representation is quite
 * convenient in practice. It also allows us to manipulate large (or possibly
 * infinite) variable sets with sparse assignments of non-Top values.
 */
template <typename Variable,
          typename Domain,
          typename VariableHash = std::hash<Variable>,
          typename VariableEqual = std::equal_to<Variable>>
class HashedAbstractEnvironment final
    : public AbstractDomainScaffolding<
          hae_impl::MapValue<Variable, Domain, VariableHash, VariableEqual>,
          HashedAbstractEnvironment<Variable,
                                    Domain,
                                    VariableHash,
                                    VariableEqual>> {
 public:
  using Value =
      hae_impl::MapValue<Variable, Domain, VariableHash, VariableEqual>;

  /*
   * The default constructor produces the Top value.
   */
  HashedAbstractEnvironment()
      : AbstractDomainScaffolding<Value, HashedAbstractEnvironment>() {}

  explicit HashedAbstractEnvironment(AbstractValueKind kind)
      : AbstractDomainScaffolding<Value, HashedAbstractEnvironment>(kind) {}

  explicit HashedAbstractEnvironment(
      std::initializer_list<std::pair<Variable, Domain>> l) {
    for (const auto& p : l) {
      if (p.second.is_bottom()) {
        this->set_to_bottom();
        return;
      }
      this->get_value()->insert_binding(p.first, p.second);
    }
    this->normalize();
  }

  bool is_value() const { return this->kind() == AbstractValueKind::Value; }

  size_t size() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->m_map.size();
  }

  const std::unordered_map<Variable, Domain, VariableHash, VariableEqual>&
  bindings() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->m_map.bindings();
  }

  const Domain& get(const Variable& variable) const {
    if (this->is_bottom()) {
      static const Domain bottom = Domain::bottom();
      return bottom;
    }
    return this->get_value()->m_map.at(variable);
  }

  HashedAbstractEnvironment& set(const Variable& variable,
                                 const Domain& value) {
    return set_internal(variable, value);
  }

  HashedAbstractEnvironment& set(const Variable& variable, Domain&& value) {
    return set_internal(variable, std::move(value));
  }

  template <typename Operation> // void (Domain*)
  HashedAbstractEnvironment& update(const Variable& variable,
                                    Operation&& operation) {
    if (this->is_bottom()) {
      return *this;
    }
    try {
      this->get_value()->m_map.update(
          [operation = std::forward<Operation>(operation)](Domain* value) {
            operation(value);
            if (value->is_bottom()) {
              throw hae_impl::value_is_bottom();
            }
          },
          variable);
    } catch (const hae_impl::value_is_bottom&) {
      this->set_to_bottom();
      return *this;
    }
    this->normalize();
    return *this;
  }

  template <typename Visitor> // void(const std::pair<Variable, Domain>&)
  void visit(Visitor&& visitor) const {
    if (this->is_bottom()) {
      return;
    }
    this->get_value()->m_map.visit(std::forward<Visitor>(visitor));
  }

  static HashedAbstractEnvironment bottom() {
    return HashedAbstractEnvironment(AbstractValueKind::Bottom);
  }

  static HashedAbstractEnvironment top() {
    return HashedAbstractEnvironment(AbstractValueKind::Top);
  }

 private:
  template <typename D>
  HashedAbstractEnvironment& set_internal(const Variable& variable, D&& value) {
    if (this->is_bottom()) {
      return *this;
    }
    if (value.is_bottom()) {
      this->set_to_bottom();
      return *this;
    }
    this->get_value()->insert_binding(variable, std::forward<D>(value));
    this->normalize();
    return *this;
  }
};

} // namespace sparta

template <typename Variable,
          typename Domain,
          typename VariableHash,
          typename VariableEqual>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::HashedAbstractEnvironment<Variable,
                                                     Domain,
                                                     VariableHash,
                                                     VariableEqual>& e) {
  using namespace sparta;
  switch (e.kind()) {
  case AbstractValueKind::Bottom: {
    o << "_|_";
    break;
  }
  case AbstractValueKind::Top: {
    o << "T";
    break;
  }
  case AbstractValueKind::Value: {
    o << "[#" << e.size() << "]";
    o << "{";
    auto& bindings = e.bindings();
    for (auto it = bindings.begin(); it != bindings.end();) {
      o << it->first << " -> " << it->second;
      ++it;
      if (it != bindings.end()) {
        o << ", ";
      }
    }
    o << "}";
    break;
  }
  }
  return o;
}

namespace sparta {

namespace hae_impl {

/*
 * The definition of an element of an abstract environment, i.e., a map from a
 * (possibly infinite) set of variables to an abstract domain implemented as a
 * hashtable. Variable bindings with the Top value are not stored in the
 * hashtable. The hashtable can never contain bindings with Bottom, as those are
 * filtered out in HashedAbstractEnvironment (the whole environment is set to
 * Bottom in that case). The Meet and Narrowing operations abort and return
 * AbstractValueKind::Bottom whenever a binding with Bottom is about to be
 * created.
 */
template <typename Variable,
          typename Domain,
          typename VariableHash,
          typename VariableEqual>
class MapValue final
    : public AbstractValue<
          MapValue<Variable, Domain, VariableHash, VariableEqual>> {
 public:
  struct ValueInterface final : public AbstractMapValue<ValueInterface> {
    using type = Domain;

    static type default_value() { return type::top(); }

    static bool is_default_value(const type& x) { return x.is_top(); }

    static bool equals(const type& x, const type& y) { return x.equals(y); }

    static bool leq(const type& x, const type& y) { return x.leq(y); }

    constexpr static AbstractValueKind default_value_kind =
        AbstractValueKind::Top;
  };

  using MapType =
      HashMap<Variable, Domain, ValueInterface, VariableHash, VariableEqual>;

  MapValue() = default;

  MapValue(const Variable& variable, Domain value) {
    insert_binding(variable, std::move(value));
  }

  void clear() { m_map.clear(); }

  AbstractValueKind kind() const {
    // If the map is empty, then all variables are implicitly bound to Top,
    // i.e., the abstract environment itself is Top.
    return (m_map.size() == 0) ? AbstractValueKind::Top
                               : AbstractValueKind::Value;
  }

  bool leq(const MapValue& other) const { return m_map.leq(other.m_map); }

  bool equals(const MapValue& other) const { return m_map.equals(other.m_map); }

  AbstractValueKind join_with(const MapValue& other) {
    return join_like_operation(
        other, [](Domain* x, const Domain& y) { x->join_with(y); });
  }

  AbstractValueKind widen_with(const MapValue& other) {
    return join_like_operation(
        other, [](Domain* x, const Domain& y) { x->widen_with(y); });
  }

  AbstractValueKind meet_with(const MapValue& other) {
    return meet_like_operation(
        other, [](Domain* x, const Domain& y) { x->meet_with(y); });
  }

  AbstractValueKind narrow_with(const MapValue& other) {
    return meet_like_operation(
        other, [](Domain* x, const Domain& y) { x->narrow_with(y); });
  }

 private:
  template <typename D>
  void insert_binding(const Variable& variable, D&& value) {
    // The Bottom value is handled in HashedAbstractEnvironment and should
    // never occur here.
    RUNTIME_CHECK(!value.is_bottom(), internal_error());
    m_map.insert_or_assign(variable, std::forward<D>(value));
  }

  template <typename Operation> // void(Domain*, const Domain&)
  AbstractValueKind join_like_operation(const MapValue& other,
                                        Operation&& operation) {
    m_map.intersection_with(std::forward<Operation>(operation), other.m_map);
    return kind();
  }

  template <typename Operation> // void(Domain*, const Domain&)
  AbstractValueKind meet_like_operation(const MapValue& other,
                                        Operation&& operation) {
    try {
      m_map.union_with(
          [operation = std::forward<Operation>(operation)](
              Domain* left, const Domain& right) {
            operation(left, right);
            if (left->is_bottom()) {
              throw value_is_bottom();
            }
          },
          other.m_map);
    } catch (const value_is_bottom&) {
      clear();
      return AbstractValueKind::Bottom;
    }
    return kind();
  }

  MapType m_map;

  template <typename T1, typename T2, typename T3, typename T4>
  friend class sparta::HashedAbstractEnvironment;
};

} // namespace hae_impl

} // namespace sparta
