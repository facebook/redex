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
#include <sparta/PatriciaTreeHashMap.h>

namespace sparta {

namespace pthmae_impl {

template <typename Variable, typename Domain>
class MapValue;

class value_is_bottom {};

} // namespace pthmae_impl

/*
 * An abstract environment based on `PatriciaTreeHashMap`.
 *
 * See `PatriciaTreeMapAbstractEnvironment` for more information.
 */
template <typename Variable, typename Domain>
class PatriciaTreeHashMapAbstractEnvironment final
    : public AbstractDomainScaffolding<
          pthmae_impl::MapValue<Variable, Domain>,
          PatriciaTreeHashMapAbstractEnvironment<Variable, Domain>> {
 public:
  using Value = pthmae_impl::MapValue<Variable, Domain>;

  using MapType =
      PatriciaTreeHashMap<Variable, Domain, typename Value::ValueInterface>;

  /*
   * The default constructor produces the Top value.
   */
  PatriciaTreeHashMapAbstractEnvironment()
      : AbstractDomainScaffolding<Value,
                                  PatriciaTreeHashMapAbstractEnvironment>() {}

  explicit PatriciaTreeHashMapAbstractEnvironment(AbstractValueKind kind)
      : AbstractDomainScaffolding<Value,
                                  PatriciaTreeHashMapAbstractEnvironment>(
            kind) {}

  explicit PatriciaTreeHashMapAbstractEnvironment(
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

  size_t size() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->m_map.size();
  }

  const MapType& bindings() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->m_map;
  }

  const Domain& get(const Variable& variable) const {
    if (this->is_bottom()) {
      static const Domain bottom = Domain::bottom();
      return bottom;
    }
    return this->get_value()->m_map.at(variable);
  }

  PatriciaTreeHashMapAbstractEnvironment& set(const Variable& variable,
                                              const Domain& value) {
    return set_internal(variable, value);
  }

  PatriciaTreeHashMapAbstractEnvironment& set(const Variable& variable,
                                              Domain&& value) {
    return set_internal(variable, std::move(value));
  }

  template <typename Operation> // void(Domain*)
  bool transform(Operation&& f) {
    if (this->is_bottom()) {
      return false;
    }
    bool res = this->get_value()->transform(std::forward<Operation>(f));
    this->normalize();
    return res;
  }

  template <typename Visitor> // void(const std::pair<Variable, Domain>&)
  void visit(Visitor&& visitor) const {
    if (this->is_bottom()) {
      return;
    }
    this->get_value()->visit(std::forward<Visitor>(visitor));
  }

  PatriciaTreeHashMapAbstractEnvironment& clear() {
    if (this->is_bottom()) {
      return *this;
    }
    this->get_value()->clear();
    this->normalize();
    return *this;
  }

  template <typename Operation> // void(Domain*)
  PatriciaTreeHashMapAbstractEnvironment& update(const Variable& variable,
                                                 Operation&& operation) {
    if (this->is_bottom()) {
      return *this;
    }
    try {
      this->get_value()->m_map.update(
          [operation = std::forward<Operation>(operation)](Domain* x) -> void {
            operation(x);
            if (x->is_bottom()) {
              throw pthmae_impl::value_is_bottom();
            }
          },
          variable);
    } catch (const pthmae_impl::value_is_bottom&) {
      this->set_to_bottom();
    }
    this->normalize();
    return *this;
  }

  static PatriciaTreeHashMapAbstractEnvironment bottom() {
    return PatriciaTreeHashMapAbstractEnvironment(AbstractValueKind::Bottom);
  }

  static PatriciaTreeHashMapAbstractEnvironment top() {
    return PatriciaTreeHashMapAbstractEnvironment(AbstractValueKind::Top);
  }

 private:
  template <typename D>
  PatriciaTreeHashMapAbstractEnvironment& set_internal(const Variable& variable,
                                                       D&& value) {
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

template <typename Variable, typename Domain>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::PatriciaTreeHashMapAbstractEnvironment<Variable,
                                                                  Domain>& e) {
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
    o << e.bindings();
    break;
  }
  }
  return o;
}

namespace sparta {

namespace pthmae_impl {

/*
 * The definition of an element of an abstract environment, i.e., a map from a
 * (possibly infinite) set of variables to an abstract domain implemented as a
 * hashtable. Variable bindings with the Top value are not stored in the
 * hashtable. The hashtable can never contain bindings with Bottom, as those are
 * filtered out in PatriciaTreeHashMapAbstractEnvironment (the whole environment
 * is set to Bottom in that case). The Meet and Narrowing operations abort and
 * return AbstractValueKind::Bottom whenever a binding with Bottom is about to
 * be created.
 */
template <typename Variable, typename Domain>
class MapValue final : public AbstractValue<MapValue<Variable, Domain>> {
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

  MapValue() = default;

  MapValue(const Variable& variable, Domain value) {
    insert_binding(variable, std::move(value));
  }

  void clear() { m_map.clear(); }

  AbstractValueKind kind() const {
    // If the map is empty, then all variables are implicitly bound to Top,
    // i.e., the abstract environment itself is Top.
    return m_map.empty() ? AbstractValueKind::Top : AbstractValueKind::Value;
  }

  bool leq(const MapValue& other) const { return m_map.leq(other.m_map); }

  bool equals(const MapValue& other) const { return m_map.equals(other.m_map); }

  AbstractValueKind join_with(const MapValue& other) {
    return join_like_operation(
        other, [](Domain* x, const Domain& y) { return x->join_with(y); });
  }

  AbstractValueKind widen_with(const MapValue& other) {
    return join_like_operation(
        other, [](Domain* x, const Domain& y) { return x->widen_with(y); });
  }

  AbstractValueKind meet_with(const MapValue& other) {
    return meet_like_operation(
        other, [](Domain* x, const Domain& y) { return x->meet_with(y); });
  }

  AbstractValueKind narrow_with(const MapValue& other) {
    return meet_like_operation(
        other, [](Domain* x, const Domain& y) { return x->narrow_with(y); });
  }

 private:
  void insert_binding(const Variable& variable, Domain value) {
    // The Bottom value is handled by the caller and should never occur here.
    RUNTIME_CHECK(!value.is_bottom(), internal_error());
    m_map.insert_or_assign(variable, std::move(value));
  }

  template <typename Operation> // void(Domain*)
  bool transform(Operation&& f) {
    return m_map.transform(std::forward<Operation>(f));
  }

  template <typename Visitor> // void(const std::pair<Variable, Domain>&)
  void visit(Visitor&& visitor) const {
    return m_map.visit(std::forward<Visitor>(visitor));
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
          [operation = std::forward<Operation>(operation)](Domain* x,
                                                           const Domain& y) {
            operation(x, y);
            if (x->is_bottom()) {
              throw value_is_bottom();
            }
          },
          other.m_map);
      return kind();
    } catch (const value_is_bottom&) {
      clear();
      return AbstractValueKind::Bottom;
    }
  }

  PatriciaTreeHashMap<Variable, Domain, ValueInterface> m_map;

  template <typename T1, typename T2>
  friend class sparta::PatriciaTreeHashMapAbstractEnvironment;
};

} // namespace pthmae_impl

} // namespace sparta
