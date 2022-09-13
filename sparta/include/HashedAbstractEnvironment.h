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
#include <unordered_map>
#include <utility>

#include "AbstractDomain.h"

namespace sparta {

namespace hae_impl {

template <typename Variable,
          typename Domain,
          typename VariableHash,
          typename VariableEqual>
class MapValue;

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

  HashedAbstractEnvironment(AbstractValueKind kind)
      : AbstractDomainScaffolding<Value, HashedAbstractEnvironment>(kind) {}

  HashedAbstractEnvironment(
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
    return this->get_value()->m_map;
  }

  const Domain& get(const Variable& variable) const {
    if (this->is_bottom()) {
      static const Domain bottom = Domain::bottom();
      return bottom;
    }
    auto binding = this->get_value()->m_map.find(variable);
    if (binding == this->get_value()->m_map.end()) {
      static const Domain top = Domain::top();
      return top;
    }
    return binding->second;
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
    auto& map = this->get_value()->m_map;
    auto binding = map.find(variable);
    Domain* value;
    if (binding == map.end()) {
      // This means it's an implicit binding (variable, Top). We explicitly
      // construct the Top value in order to apply the operation.
      value = &map[variable];
      value->set_to_top();
    } else {
      value = &binding->second;
    }
    operation(value);
    // We normalize the abstract environment after the operation has been
    // completed.
    if (value->is_bottom()) {
      this->set_to_bottom();
      return *this;
    }
    if (value->is_top()) {
      map.erase(variable);
    }
    this->normalize();
    return *this;
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
  MapValue() = default;

  MapValue(const Variable& variable, Domain value) {
    insert_binding(variable, std::move(value));
  }

  void clear() override { m_map.clear(); }

  AbstractValueKind kind() const override {
    // If the map is empty, then all variables are implicitly bound to Top,
    // i.e., the abstract environment itself is Top.
    return (m_map.size() == 0) ? AbstractValueKind::Top
                               : AbstractValueKind::Value;
  }

  bool leq(const MapValue& other) const override {
    if (other.m_map.size() > m_map.size()) {
      // In this case, there is a variable bound to a non-Top value in 'other'
      // that is not defined in 'this' (and is therefore implicitly bound to
      // Top).
      return false;
    }
    for (const auto& binding : m_map) {
      auto it = other.m_map.find(binding.first);
      if (it == other.m_map.end()) {
        // The other value is Top.
        continue;
      }
      if (!binding.second.leq(it->second)) {
        return false;
      }
    }
    // Now we look for a variable appearing in 'other' that is not defined in
    // 'this' (and thus bound to Top).
    for (const auto& binding : other.m_map) {
      auto it = m_map.find(binding.first);
      if (it == m_map.end()) {
        // The value is Top, but we know by construction that binding.second is
        // not Top.
        return false;
      }
    }
    return true;
  }

  bool equals(const MapValue& other) const override {
    if (m_map.size() != other.m_map.size()) {
      return false;
    }
    for (const auto& binding : m_map) {
      auto it = other.m_map.find(binding.first);
      if (it == other.m_map.end()) {
        return false;
      }
      if (!binding.second.equals(it->second)) {
        return false;
      }
    }
    return true;
  }

  AbstractValueKind join_with(const MapValue& other) override {
    return join_like_operation(
        other, [](Domain* x, const Domain& y) { x->join_with(y); });
  }

  AbstractValueKind widen_with(const MapValue& other) override {
    return join_like_operation(
        other, [](Domain* x, const Domain& y) { x->widen_with(y); });
  }

  AbstractValueKind meet_with(const MapValue& other) override {
    return meet_like_operation(
        other, [](Domain* x, const Domain& y) { x->meet_with(y); });
  }

  AbstractValueKind narrow_with(const MapValue& other) override {
    return meet_like_operation(
        other, [](Domain* x, const Domain& y) { x->narrow_with(y); });
  }

 private:
  template <typename D>
  void insert_binding(const Variable& variable, D&& value) {
    // The Bottom value is handled in HashedAbstractEnvironment and should
    // never occur here.
    RUNTIME_CHECK(!value.is_bottom(), internal_error());
    if (value.is_top()) {
      // Bindings with the Top value are not explicitly represented.
      m_map.erase(variable);
    } else {
      m_map.insert_or_assign(variable, std::forward<D>(value));
    }
  }

  template <typename Operation> // void(Domain*, const Domain&)
  AbstractValueKind join_like_operation(const MapValue& other,
                                        Operation&& operation) {
    for (auto it = m_map.begin(); it != m_map.end();) {
      auto other_binding = other.m_map.find(it->first);
      if (other_binding == other.m_map.end()) {
        // The other value is Top, we just erase the binding. We need to use a
        // different iterator, because all iterators to an erased binding are
        // invalidated.
        auto to_erase = it++;
        m_map.erase(to_erase);
      } else {
        // We compute the join-like combination of the values.
        operation(&it->second, other_binding->second);
        if (it->second.is_top()) {
          // If the result is Top, we erase the binding.
          auto to_erase = it++;
          m_map.erase(to_erase);
        } else {
          ++it;
        }
      }
    }
    return kind();
  }

  template <typename Operation> // void(Domain*, const Domain&)
  AbstractValueKind meet_like_operation(const MapValue& other,
                                        Operation&& operation) {
    for (const auto& other_binding : other.m_map) {
      auto binding = m_map.find(other_binding.first);
      if (binding == m_map.end()) {
        // The value is Top, we just insert the other value (Top is the identity
        // for meet-like operations).
        m_map[other_binding.first] = other_binding.second;
      } else {
        // We compute the meet-like combination of the values.
        operation(&binding->second, other_binding.second);
        if (binding->second.is_bottom()) {
          // If the result is Bottom, the entire environment becomes Bottom.
          clear();
          return AbstractValueKind::Bottom;
        }
      }
    }
    return kind();
  }

  std::unordered_map<Variable, Domain, VariableHash, VariableEqual> m_map;

  template <typename T1, typename T2, typename T3, typename T4>
  friend class sparta::HashedAbstractEnvironment;
};

} // namespace hae_impl

} // namespace sparta
