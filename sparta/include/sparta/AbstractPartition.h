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

namespace sparta {
namespace ap_impl {
template <typename Map>
class AbstractPartitionStaticAssert;
}

/*
 * An abstract partition based on a given abstract map.
 *
 * A partition is a mapping from a set of labels to elements in an abstract
 * domain. It denotes a union of properties. A partition is Bottom iff all its
 * bindings are set to Bottom, and it is Top iff all its bindings are set to
 * Top.
 *
 * All lattice operations are applied componentwise.
 *
 * In order to minimize the size of the underlying map, we do not explicitly
 * represent bindings of a label to the Bottom element.
 *
 * This implementation differs slightly from the textbook definition of a
 * partition: our Top partition cannot have its labels re-bound to anything
 * other than Top. I.e. for all labels L and domains D,
 *
 *   AbstractPartition::top().set(L, D) == AbstractPartition::top()
 *
 * This makes for a much simpler implementation.
 */
template <typename Map>
class AbstractPartition final
    : public AbstractDomain<AbstractPartition<Map>>,
      private ap_impl::AbstractPartitionStaticAssert<Map> {
 public:
  using Label = typename Map::key_type;
  using Domain = typename Map::mapped_type;
  using MapType = Map;

  /*
   * The default constructor produces the Bottom value.
   */
  AbstractPartition() = default;

  explicit AbstractPartition(
      std::initializer_list<std::pair<Label, Domain>> l) {
    for (const auto& p : l) {
      set(p.first, p.second);
    }
  }

  /*
   * Number of bindings not set to Bottom. This operation is not defined if the
   * AbstractPartition is set to Top.
   */
  size_t size() const {
    SPARTA_RUNTIME_CHECK(!is_top(), undefined_operation());
    return m_map.size();
  }

  /*
   * Get the bindings that are not set to Bottom.
   * This operation is not defined if the partition is set to Top.
   */
  const MapType& bindings() const {
    SPARTA_RUNTIME_CHECK(!is_top(), undefined_operation());
    return m_map;
  }

  const Domain& get(const Label& label) const {
    if (is_top()) {
      static const Domain top = Domain::top();
      return top;
    }
    return m_map.at(label);
  }

  /*
   * This is a no-op if the partition is set to Top.
   */
  AbstractPartition& set(const Label& label, const Domain& value) {
    return set_internal(label, value);
  }

  /*
   * This is a no-op if the partition is set to Top.
   */
  AbstractPartition& set(const Label& label, Domain&& value) {
    return set_internal(label, std::move(value));
  }

  /*
   * This is a no-op if the partition is set to Top.
   */

  template <typename Operation> // See `Map::update`
  AbstractPartition& update(const Label& label, Operation&& operation) {
    if (is_top()) {
      return *this;
    }
    m_map.update(std::forward<Operation>(operation), label);
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
      if (is_top()) {
        return false;
      }
      return m_map.transform(std::forward<Operation>(f));
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      if (is_top()) {
        return;
      }
      m_map.transform(std::forward<Operation>(f));
    }
  }

  template <typename Visitor> // void(const std::pair<Label, Domain>&)
  void visit(Visitor&& visitor) const {
    if (is_top()) {
      return;
    }
    m_map.visit(std::forward<Visitor>(visitor));
  }

  bool is_top() const { return m_is_top; }

  bool is_bottom() const { return !m_is_top && m_map.empty(); }

  void set_to_bottom() {
    m_map.clear();
    m_is_top = false;
  }

  void set_to_top() {
    m_map.clear();
    m_is_top = true;
  }

  bool leq(const AbstractPartition& other) const {
    if (is_top()) {
      return other.is_top();
    }
    if (other.is_top()) {
      return true;
    }
    return m_map.leq(other.m_map);
  }

  bool equals(const AbstractPartition& other) const {
    if (m_is_top != other.m_is_top) {
      return false;
    }
    return m_map.equals(other.m_map);
  }

  void join_with(const AbstractPartition& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      join_like_operation(
          other,
          [](const Domain& x, const Domain& y) -> Domain { return x.join(y); });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      join_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->join_with(y); });
    }
  }

  void widen_with(const AbstractPartition& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      join_like_operation(other,
                          [](const Domain& x, const Domain& y) -> Domain {
                            return x.widening(y);
                          });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      join_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->widen_with(y); });
    }
  }

  void meet_with(const AbstractPartition& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      meet_like_operation(
          other,
          [](const Domain& x, const Domain& y) -> Domain { return x.meet(y); });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      meet_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->meet_with(y); });
    }
  }

  void narrow_with(const AbstractPartition& other) {
    if constexpr (Map::mutability == AbstractMapMutability::Immutable) {
      meet_like_operation(other,
                          [](const Domain& x, const Domain& y) -> Domain {
                            return x.narrowing(y);
                          });
    } else if constexpr (Map::mutability == AbstractMapMutability::Mutable) {
      meet_like_operation(
          other, [](Domain* x, const Domain& y) -> void { x->narrow_with(y); });
    }
  }

  template <typename Operation> // See `Map::union_with`
  void join_like_operation(const AbstractPartition& other,
                           Operation&& operation) {
    if (is_top()) {
      return;
    }
    if (other.is_top()) {
      set_to_top();
      return;
    }
    m_map.union_with(std::forward<Operation>(operation), other.m_map);
  }

  template <typename Operation> // See `Map::intersection_with`
  void meet_like_operation(const AbstractPartition& other,
                           Operation&& operation) {
    if (is_top()) {
      *this = other;
      return;
    }
    if (other.is_top()) {
      return;
    }
    m_map.intersection_with(std::forward<Operation>(operation), other.m_map);
  }

  template <typename Operation> // See `Map::difference_like_operation`
  void difference_like_operation(const AbstractPartition& other,
                                 Operation&& operation) {
    if (other.is_top()) {
      set_to_bottom();
    } else if (is_top()) {
      return;
    } else {
      m_map.difference_with(std::forward<Operation>(operation), other.m_map);
    }
  }

  static AbstractPartition bottom() { return AbstractPartition(); }

  static AbstractPartition top() {
    auto part = AbstractPartition();
    part.m_is_top = true;
    return part;
  }

 private:
  template <typename D>
  AbstractPartition& set_internal(const Label& label, D&& value) {
    if (is_top()) {
      return *this;
    }
    m_map.insert_or_assign(label, std::forward<D>(value));
    return *this;
  }

  Map m_map;
  bool m_is_top{false};
};

template <typename Domain>
struct BottomValueInterface final
    : public AbstractMapValue<BottomValueInterface<Domain>> {
  using type = Domain;

  static type default_value() { return type::bottom(); }

  static bool is_default_value(const type& x) { return x.is_bottom(); }

  static bool equals(const type& x, const type& y) { return x.equals(y); }

  static bool leq(const type& x, const type& y) { return x.leq(y); }

  constexpr static AbstractValueKind default_value_kind =
      AbstractValueKind::Bottom;
};

namespace ap_impl {
template <typename Map>
class AbstractPartitionStaticAssert {
 protected:
  ~AbstractPartitionStaticAssert() {
    static_assert(std::is_base_of<AbstractMap<Map>, Map>::value,
                  "Map doesn't inherit from AbstractMap");

    using ValueInterface = typename Map::value_interface;
    static_assert(std::is_base_of<AbstractMapValue<ValueInterface>,
                                  ValueInterface>::value,
                  "ValueInterface doesn't inherit from AbstractMapValue");
    static_assert(ValueInterface::default_value_kind ==
                      AbstractValueKind::Bottom,
                  "ValueInterface::default_value_kind is not Bottom");
  }
};
} // namespace ap_impl

} // namespace sparta

template <typename Map>
inline std::ostream& operator<<(
    std::ostream& o, const typename sparta::AbstractPartition<Map>& partition) {
  if (partition.is_bottom()) {
    o << "_|_";
  } else if (partition.is_top()) {
    o << "T";
  } else {
    o << "[#" << partition.size() << "]";
    o << partition.bindings();
  }
  return o;
}
