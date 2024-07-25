/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <utility>

#include <sparta/AbstractDomain.h>
#include <sparta/AbstractMap.h>
#include <sparta/AbstractMapValue.h>
#include <sparta/PerfectForwardCapture.h>

namespace sparta {

namespace environment_impl {

template <typename Map>
class AbstractEnvironmentStaticAssert;

template <typename Map>
class MapValue;

class value_is_bottom {};

} // namespace environment_impl

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
 * In order to minimize the size of the underlying map, we do not explicitly
 * represent bindings of a variable to the Top element. Hence, any variable that
 * is not explicitly represented in the environment has a default value of Top.
 * This representation is quite convenient in practice. It also allows us to
 * manipulate large (or possibly infinite) variable sets with sparse assignments
 * of non-Top values.
 */
template <typename Map>
class AbstractEnvironment final
    : public AbstractDomainScaffolding<environment_impl::MapValue<Map>,
                                       AbstractEnvironment<Map>>,
      private environment_impl::AbstractEnvironmentStaticAssert<Map> {
 public:
  using Variable = typename Map::key_type;
  using Domain = typename Map::mapped_type;
  using MapType = Map;

  /*
   * The default constructor produces the Top value.
   */
  AbstractEnvironment()
      : AbstractDomainScaffolding<environment_impl::MapValue<Map>,
                                  AbstractEnvironment>() {}

  explicit AbstractEnvironment(AbstractValueKind kind)
      : AbstractDomainScaffolding<environment_impl::MapValue<Map>,
                                  AbstractEnvironment>(kind) {}

  explicit AbstractEnvironment(
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
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->m_map.size();
  }

  const MapType& bindings() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
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

  AbstractEnvironment& set(const Variable& variable, const Domain& value) {
    return set_internal(variable, value);
  }

  AbstractEnvironment& set(const Variable& variable, Domain&& value) {
    return set_internal(variable, std::move(value));
  }

  template <typename Operation> // See `Map::update`
  AbstractEnvironment& update(const Variable& variable, Operation&& operation) {
    if (this->is_bottom()) {
      return *this;
    }
    try {
      if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
        this->get_value()->m_map.update(
            [operation = fwd_capture(std::forward<Operation>(operation))](
                const Domain& value) mutable -> Domain {
              Domain result = operation.get()(value);
              if (result.is_bottom()) {
                throw environment_impl::value_is_bottom();
              }
              return result;
            },
            variable);
      } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
        this->get_value()->m_map.update(
            [operation = fwd_capture(std::forward<Operation>(operation))](
                Domain* value) mutable -> void {
              operation.get()(value);
              if (value->is_bottom()) {
                throw environment_impl::value_is_bottom();
              }
            },
            variable);
      }
    } catch (const environment_impl::value_is_bottom&) {
      this->set_to_bottom();
      return *this;
    }
    this->normalize();
    return *this;
  }

  using TransformResult =
      typename std::conditional<Map::mutability ==
                                    AbstractMapMutability::Immutable,
                                bool,
                                void>::type;

  template <typename Operation> // See `Map::transform`
  TransformResult transform(Operation&& f) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      if (this->is_bottom()) {
        return false;
      }
      bool res = this->get_value()->m_map.transform(std::forward<Operation>(f));
      this->normalize();
      return res;
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      if (this->is_bottom()) {
        return;
      }
      this->get_value()->m_map.transform(std::forward<Operation>(f));
      this->normalize();
      return;
    }
  }

  bool erase_all_matching(const Variable& variable_mask) {
    if (this->is_bottom()) {
      return false;
    }
    bool res = this->get_value()->m_map.erase_all_matching(variable_mask);
    this->normalize();
    return res;
  }

  template <typename Visitor> // void(const std::pair<Variable, Domain>&)
  void visit(Visitor&& visitor) const {
    if (this->is_bottom()) {
      return;
    }
    this->get_value()->m_map.visit(std::forward<Visitor>(visitor));
  }

  static AbstractEnvironment bottom() {
    return AbstractEnvironment(AbstractValueKind::Bottom);
  }

  static AbstractEnvironment top() {
    return AbstractEnvironment(AbstractValueKind::Top);
  }

 private:
  template <typename D>
  AbstractEnvironment& set_internal(const Variable& variable, D&& value) {
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

namespace environment_impl {

template <typename Map>
class AbstractEnvironmentStaticAssert {
 protected:
  ~AbstractEnvironmentStaticAssert() {
    static_assert(std::is_base_of<AbstractMap<Map>, Map>::value,
                  "Map doesn't inherit from AbstractMap");

    using ValueInterface = typename Map::value_interface;
    static_assert(std::is_base_of<AbstractMapValue<ValueInterface>,
                                  ValueInterface>::value,
                  "ValueInterface doesn't inherit from AbstractMapValue");
    static_assert(ValueInterface::default_value_kind == AbstractValueKind::Top,
                  "ValueInterface::default_value_kind is not Top");
  }
};

/*
 * The definition of an element of an abstract environment, i.e., a map from a
 * (possibly infinite) set of variables to an abstract domain implemented as a
 * hashtable. Variable bindings with the Top value are not stored in the
 * hashtable. The map can never contain bindings with Bottom, as those are
 * filtered out in AbstractEnvironment (the whole environment is set to
 * Bottom in that case). The Meet and Narrowing operations abort and return
 * AbstractValueKind::Bottom whenever a binding with Bottom is about to be
 * created.
 */
template <typename Map>
class MapValue final : public AbstractValue<MapValue<Map>> {
 public:
  using Variable = typename Map::key_type;
  using Domain = typename Map::mapped_type;

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
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      return join_like_operation(
          other,
          [](const Domain& x, const Domain& y) -> Domain { return x.join(y); });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      return join_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->join_with(y); });
    }
  }

  AbstractValueKind widen_with(const MapValue& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      return join_like_operation(
          other, [](const Domain& x, const Domain& y) -> Domain {
            return x.widening(y);
          });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      return join_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->widen_with(y); });
    }
  }

  AbstractValueKind meet_with(const MapValue& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      return meet_like_operation(
          other,
          [](const Domain& x, const Domain& y) -> Domain { return x.meet(y); });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      return meet_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->meet_with(y); });
    }
  }

  AbstractValueKind narrow_with(const MapValue& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      return meet_like_operation(
          other, [](const Domain& x, const Domain& y) -> Domain {
            return x.narrowing(y);
          });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      return meet_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->narrow_with(y); });
    }
  }

 private:
  template <typename D>
  void insert_binding(const Variable& variable, D&& value) {
    // The Bottom value is handled in AbstractEnvironment and should
    // never occur here.
    SPARTA_RUNTIME_CHECK(!value.is_bottom(), internal_error());
    m_map.insert_or_assign(variable, std::forward<D>(value));
  }

  template <typename Operation>
  AbstractValueKind join_like_operation(const MapValue& other,
                                        const Operation& operation) {
    m_map.intersection_with(operation, other.m_map);
    return kind();
  }

  template <typename Operation>
  AbstractValueKind meet_like_operation(const MapValue& other,
                                        const Operation& operation) {
    try {
      if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
        m_map.union_with(
            [&operation](const Domain& x, const Domain& y) -> Domain {
              Domain result = operation(x, y);
              if (result.is_bottom()) {
                throw value_is_bottom();
              }
              return result;
            },
            other.m_map);
      } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
        m_map.union_with(
            [&operation](Domain* x, const Domain& y) -> void {
              operation(x, y);
              if (x->is_bottom()) {
                throw value_is_bottom();
              }
            },
            other.m_map);
      }
    } catch (const value_is_bottom&) {
      clear();
      return AbstractValueKind::Bottom;
    }
    return kind();
  }

 private:
  Map m_map;

  template <typename T>
  friend class sparta::AbstractEnvironment;
};

} // namespace environment_impl

template <typename Domain>
struct TopValueInterface final
    : public AbstractMapValue<TopValueInterface<Domain>> {
  using type = Domain;

  static type default_value() { return type::top(); }

  static bool is_default_value(const type& x) { return x.is_top(); }

  static bool equals(const type& x, const type& y) { return x.equals(y); }

  static bool leq(const type& x, const type& y) { return x.leq(y); }

  constexpr static AbstractValueKind default_value_kind =
      AbstractValueKind::Top;
};

} // namespace sparta

template <typename Map>
inline std::ostream& operator<<(
    std::ostream& o, const typename sparta::AbstractEnvironment<Map>& e) {
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
