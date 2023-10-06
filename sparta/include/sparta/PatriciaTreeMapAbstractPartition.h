/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ostream>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <utility>

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeMap.h>

namespace sparta {

/*
 * An abstract partition based on Patricia trees that is cheap to copy.
 *
 * In order to minimize the size of the underlying tree, we do not explicitly
 * represent bindings of a label to the Bottom element.
 *
 * See HashedAbstractPartition.h for more details about abstract partitions.
 *
 * This implementation differs slightly from the textbook definition of a
 * partition: our Top partition cannot have its labels re-bound to anything
 * other than Top. I.e. for all labels L and domains D,
 *
 *   PatriciaTreeMapAbstractPartition::top().set(L, D) ==
 * PatriciaTreeMapAbstractPartition::top()
 *
 * This makes for a much simpler implementation.
 */
template <typename Label, typename Domain>
class PatriciaTreeMapAbstractPartition final
    : public AbstractDomain<PatriciaTreeMapAbstractPartition<Label, Domain>> {
 public:
  struct ValueInterface {
    using type = Domain;

    static type default_value() { return type::bottom(); }

    static bool is_default_value(const type& x) { return x.is_bottom(); }

    static bool equals(const type& x, const type& y) { return x.equals(y); }

    static bool leq(const type& x, const type& y) { return x.leq(y); }

    constexpr static AbstractValueKind default_value_kind =
        AbstractValueKind::Bottom;
  };

  using MapType = PatriciaTreeMap<Label, Domain, ValueInterface>;

  /*
   * The default constructor produces the Bottom value.
   */
  PatriciaTreeMapAbstractPartition() = default;

  explicit PatriciaTreeMapAbstractPartition(
      std::initializer_list<std::pair<Label, Domain>> l) {
    for (const auto& p : l) {
      set(p.first, p.second);
    }
  }

  /*
   * Number of bindings not set to Bottom. This operation is not defined if the
   * PatriciaTreeMapAbstractPartition is set to Top.
   */
  size_t size() const {
    RUNTIME_CHECK(!is_top(), undefined_operation());
    return m_map.size();
  }

  /*
   * Get the bindings that are not set to Bottom. This operation is not defined
   * if the PatriciaTreeMapAbstractPartition is set to Top.
   */
  const MapType& bindings() const {
    RUNTIME_CHECK(!is_top(), undefined_operation());
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
  PatriciaTreeMapAbstractPartition& set(const Label& label,
                                        const Domain& value) {
    return set_internal(label, value);
  }

  /*
   * This is a no-op if the partition is set to Top.
   */
  PatriciaTreeMapAbstractPartition& set(const Label& label, Domain&& value) {
    return set_internal(label, std::move(value));
  }

  /*
   * This is a no-op if the partition is set to Top.
   */

  template <typename Operation> // Domain(const Domain&)
  PatriciaTreeMapAbstractPartition& update(const Label& label,
                                           Operation&& operation) {
    if (is_top()) {
      return *this;
    }
    m_map.update(std::forward<Operation>(operation), label);
    return *this;
  }

  template <typename Operation> // Domain(const Domain&)
  bool transform(Operation&& f) {
    if (is_top()) {
      return false;
    }
    return m_map.transform(std::forward<Operation>(f));
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

  bool leq(const PatriciaTreeMapAbstractPartition& other) const {
    if (is_top()) {
      return other.is_top();
    }
    if (other.is_top()) {
      return true;
    }
    return m_map.leq(other.m_map);
  }

  bool equals(const PatriciaTreeMapAbstractPartition& other) const {
    if (m_is_top != other.m_is_top) {
      return false;
    }
    return m_map.equals(other.m_map);
  }

  void join_with(const PatriciaTreeMapAbstractPartition& other) {
    join_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.join(y); });
  }

  void widen_with(const PatriciaTreeMapAbstractPartition& other) {
    join_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.widening(y); });
  }

  void meet_with(const PatriciaTreeMapAbstractPartition& other) {
    meet_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.meet(y); });
  }

  void narrow_with(const PatriciaTreeMapAbstractPartition& other) {
    meet_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.narrowing(y); });
  }

  template <typename Operation> // Domain(const Domain&, const Domain&)
  void join_like_operation(const PatriciaTreeMapAbstractPartition& other,
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

  template <typename Operation> // Domain(const Domain&, const Domain&)
  void meet_like_operation(const PatriciaTreeMapAbstractPartition& other,
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

  template <typename Operation> // Domain(const Domain&, const Domain&)
  void difference_like_operation(const PatriciaTreeMapAbstractPartition& other,
                                 Operation&& operation) {
    if (other.is_top()) {
      set_to_bottom();
    } else if (is_top()) {
      return;
    } else {
      m_map.difference_with(std::forward<Operation>(operation), other.m_map);
    }
  }

  static PatriciaTreeMapAbstractPartition bottom() {
    return PatriciaTreeMapAbstractPartition();
  }

  static PatriciaTreeMapAbstractPartition top() {
    auto part = PatriciaTreeMapAbstractPartition();
    part.m_is_top = true;
    return part;
  }

 private:
  template <typename D>
  PatriciaTreeMapAbstractPartition& set_internal(const Label& label,
                                                 D&& value) {
    if (is_top()) {
      return *this;
    }
    m_map.insert_or_assign(label, std::forward<D>(value));
    return *this;
  }

  MapType m_map;
  bool m_is_top{false};
};

} // namespace sparta

template <typename Label, typename Domain>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::PatriciaTreeMapAbstractPartition<Label, Domain>&
        partition) {
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
