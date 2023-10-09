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
#include <utility>

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeHashMap.h>

namespace sparta {

/*
 * An abstract partition based on `PatriciaTreeHashMap`.
 *
 * See `PatriciaTreeMapAbstractPartition` for more information.
 */
template <typename Label, typename Domain>
class PatriciaTreeHashMapAbstractPartition final
    : public AbstractDomain<
          PatriciaTreeHashMapAbstractPartition<Label, Domain>> {
 public:
  struct ValueInterface final : public AbstractMapValue<ValueInterface> {
    using type = Domain;

    static type default_value() { return type::bottom(); }

    static bool is_default_value(const type& x) { return x.is_bottom(); }

    static bool equals(const type& x, const type& y) { return x.equals(y); }

    static bool leq(const type& x, const type& y) { return x.leq(y); }

    constexpr static AbstractValueKind default_value_kind =
        AbstractValueKind::Bottom;
  };

  using MapType = PatriciaTreeHashMap<Label, Domain, ValueInterface>;

  /*
   * The default constructor produces the Bottom value.
   */
  PatriciaTreeHashMapAbstractPartition() = default;

  explicit PatriciaTreeHashMapAbstractPartition(
      std::initializer_list<std::pair<Label, Domain>> l) {
    for (const auto& p : l) {
      set(p.first, p.second);
    }
  }

  /*
   * Number of bindings not set to Bottom. This operation is not defined if the
   * PatriciaTreeHashMapAbstractPartition is set to Top.
   */
  size_t size() const {
    RUNTIME_CHECK(!is_top(), undefined_operation());
    return m_map.size();
  }

  /*
   * Get the bindings that are not set to Bottom. This operation is not defined
   * if the PatriciaTreeHashMapAbstractPartition is set to Top.
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
  PatriciaTreeHashMapAbstractPartition& set(const Label& label,
                                            const Domain& value) {
    return set_internal(label, value);
  }

  /*
   * This is a no-op if the partition is set to Top.
   */
  PatriciaTreeHashMapAbstractPartition& set(const Label& label,
                                            Domain&& value) {
    return set_internal(label, std::move(value));
  }

  /*
   * This is a no-op if the partition is set to Top.
   */

  template <typename Operation> // void(Domain*)
  PatriciaTreeHashMapAbstractPartition& update(const Label& label,
                                               Operation&& operation) {
    if (is_top()) {
      return *this;
    }
    m_map.update(std::forward<Operation>(operation), label);
    return *this;
  }

  template <typename Operation> // void(Domain*)
  void transform(Operation&& f) {
    if (is_top()) {
      return;
    }
    m_map.transform(std::forward<Operation>(f));
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

  bool leq(const PatriciaTreeHashMapAbstractPartition& other) const {
    if (is_top()) {
      return other.is_top();
    }
    if (other.is_top()) {
      return true;
    }
    return m_map.leq(other.m_map);
  }

  bool equals(const PatriciaTreeHashMapAbstractPartition& other) const {
    if (m_is_top != other.m_is_top) {
      return false;
    }
    return m_map.equals(other.m_map);
  }

  void join_with(const PatriciaTreeHashMapAbstractPartition& other) {
    join_like_operation(
        other, [](Domain* x, const Domain& y) { return x->join_with(y); });
  }

  void widen_with(const PatriciaTreeHashMapAbstractPartition& other) {
    join_like_operation(
        other, [](Domain* x, const Domain& y) { return x->widen_with(y); });
  }

  void meet_with(const PatriciaTreeHashMapAbstractPartition& other) {
    meet_like_operation(
        other, [](Domain* x, const Domain& y) { return x->meet_with(y); });
  }

  void narrow_with(const PatriciaTreeHashMapAbstractPartition& other) {
    meet_like_operation(
        other, [](Domain* x, const Domain& y) { return x->narrow_with(y); });
  }

  template <typename Operation> // Domain(Domain*, const Domain&)
  void join_like_operation(const PatriciaTreeHashMapAbstractPartition& other,
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

  template <typename Operation> // void(Domain*, const Domain&)
  void meet_like_operation(const PatriciaTreeHashMapAbstractPartition& other,
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

  template <typename Operation> // void(Domain*, const Domain&)
  void difference_like_operation(
      const PatriciaTreeHashMapAbstractPartition& other,
      Operation&& operation) {
    if (other.is_top()) {
      set_to_bottom();
    } else if (is_top()) {
      return;
    } else {
      m_map.difference_with(std::forward<Operation>(operation), other.m_map);
    }
  }

  static PatriciaTreeHashMapAbstractPartition bottom() {
    return PatriciaTreeHashMapAbstractPartition();
  }

  static PatriciaTreeHashMapAbstractPartition top() {
    auto part = PatriciaTreeHashMapAbstractPartition();
    part.m_is_top = true;
    return part;
  }

 private:
  template <typename D>
  PatriciaTreeHashMapAbstractPartition& set_internal(const Label& label,
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
    const typename sparta::PatriciaTreeHashMapAbstractPartition<Label, Domain>&
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
