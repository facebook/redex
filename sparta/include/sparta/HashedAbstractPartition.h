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
#include <sparta/HashMap.h>

namespace sparta {

/*
 * A partition is a mapping from a set of labels to elements in an abstract
 * domain. It denotes a union of properties. A partition is Bottom iff all its
 * bindings are set to Bottom, and it is Top iff all its bindings are set to
 * Top.
 *
 * All lattice operations are applied componentwise.
 *
 * In order to minimize the size of the hashtable, we do not explicitly
 * represent bindings to Bottom.
 *
 * This implementation differs slightly from the textbook definition of a
 * partition: our Top partition cannot have its labels re-bound to anything
 * other than Top. I.e. for all labels L and domains D,
 *
 *   HashedAbstractPartition::top().set(L, D) == HashedAbstractPartition::top()
 *
 * This makes for a much simpler implementation.
 */
template <typename Label,
          typename Domain,
          typename LabelHash = std::hash<Label>,
          typename LabelEqual = std::equal_to<Label>>
class HashedAbstractPartition final
    : public AbstractDomain<
          HashedAbstractPartition<Label, Domain, LabelHash, LabelEqual>> {
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

  using MapType = HashMap<Label, Domain, ValueInterface, LabelHash, LabelEqual>;

  /*
   * The default constructor produces the Bottom value.
   */
  HashedAbstractPartition() = default;

  explicit HashedAbstractPartition(
      std::initializer_list<std::pair<Label, Domain>> l) {
    for (const auto& p : l) {
      set(p.first, p.second);
    }
  }

  /*
   * Number of bindings not set to Bottom. This operation is not defined if the
   * HashedAbstractPartition is set to Top.
   */
  size_t size() const {
    RUNTIME_CHECK(!is_top(), undefined_operation());
    return m_map.size();
  }

  /*
   * Get the bindings that are not set to Bottom. This operation is not defined
   * if the HashedAbstractPartition is set to Top.
   */
  const std::unordered_map<Label, Domain, LabelHash, LabelEqual>& bindings()
      const {
    RUNTIME_CHECK(!is_top(), undefined_operation());
    return m_map.bindings();
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
  HashedAbstractPartition& set(const Label& label, const Domain& value) {
    return set_internal(label, value);
  }

  /*
   * This is a no-op if the partition is set to Top.
   */
  HashedAbstractPartition& set(const Label& label, Domain&& value) {
    return set_internal(label, std::move(value));
  }

  /*
   * This is a no-op if the partition is set to Top.
   */
  template <typename Operation> // void(Domain*)
  HashedAbstractPartition& update(const Label& label, Operation&& operation) {
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

  bool leq(const HashedAbstractPartition& other) const {
    if (is_top()) {
      return other.is_top();
    }
    if (other.is_top()) {
      return true;
    }
    return m_map.leq(other.m_map);
  }

  bool equals(const HashedAbstractPartition& other) const {
    if (m_is_top != other.m_is_top) {
      return false;
    }
    return m_map.equals(other.m_map);
  }

  void join_with(const HashedAbstractPartition& other) {
    join_like_operation(other,
                        [](Domain* x, const Domain& y) { x->join_with(y); });
  }

  void widen_with(const HashedAbstractPartition& other) {
    join_like_operation(other,
                        [](Domain* x, const Domain& y) { x->widen_with(y); });
  }

  void meet_with(const HashedAbstractPartition& other) {
    meet_like_operation(other,
                        [](Domain* x, const Domain& y) { x->meet_with(y); });
  }

  void narrow_with(const HashedAbstractPartition& other) {
    meet_like_operation(other,
                        [](Domain* x, const Domain& y) { x->narrow_with(y); });
  }

  template <typename Operation> // void(Domain*, const Domain&)
  void join_like_operation(const HashedAbstractPartition& other,
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
  void meet_like_operation(const HashedAbstractPartition& other,
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
  void difference_like_operation(const HashedAbstractPartition& other,
                                 Operation&& operation) {
    if (other.is_top()) {
      set_to_bottom();
    } else if (is_top()) {
      return;
    } else {
      m_map.difference_with(std::forward<Operation>(operation), other.m_map);
    }
  }

  static HashedAbstractPartition bottom() { return HashedAbstractPartition(); }

  static HashedAbstractPartition top() {
    auto hap = HashedAbstractPartition();
    hap.m_is_top = true;
    return hap;
  }

 private:
  template <typename D>
  HashedAbstractPartition& set_internal(const Label& label, D&& value) {
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

template <typename Label,
          typename Domain,
          typename LabelHash,
          typename LabelEqual>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::
        HashedAbstractPartition<Label, Domain, LabelHash, LabelEqual>&
            partition) {
  if (partition.is_bottom()) {
    o << "_|_";
  } else if (partition.is_top()) {
    o << "T";
  } else {
    o << "[#" << partition.size() << "]";
    o << "{";
    auto& bindings = partition.bindings();
    for (auto it = bindings.begin(); it != bindings.end();) {
      o << it->first << " -> " << it->second;
      ++it;
      if (it != bindings.end()) {
        o << ", ";
      }
    }
    o << "}";
  }
  return o;
}
