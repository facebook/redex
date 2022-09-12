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
  /*
   * The default constructor produces the Bottom value.
   */
  HashedAbstractPartition() = default;

  HashedAbstractPartition(std::initializer_list<std::pair<Label, Domain>> l) {
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
    return m_map;
  }

  const Domain& get(const Label& label) const {
    if (is_top()) {
      static const Domain top = Domain::top();
      return top;
    }
    auto binding = m_map.find(label);
    if (binding == m_map.end()) {
      static const Domain bottom = Domain::bottom();
      return bottom;
    }
    return binding->second;
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
    auto binding = m_map.find(label);
    Domain* value;
    if (binding == m_map.end()) {
      // This means it's an implicit binding. We explicitly construct the
      // Bottom value in order to apply the operation.
      value = &m_map[label];
      value->set_to_bottom();
    } else {
      value = &binding->second;
    }
    operation(value);
    if (value->is_bottom()) {
      m_map.erase(label);
    }
    return *this;
  }

  bool is_top() const override { return m_is_top; }

  bool is_bottom() const override { return !m_is_top && m_map.empty(); }

  void set_to_bottom() override {
    m_map.clear();
    m_is_top = false;
  }

  void set_to_top() override {
    m_map.clear();
    m_is_top = true;
  }

  bool leq(const HashedAbstractPartition& other) const override {
    if (is_top()) {
      return other.is_top();
    }
    if (other.is_top()) {
      return true;
    }
    if (m_map.size() > other.m_map.size()) {
      // In this case, there is a label bound to a non-Bottom value in
      // 'this' that is not defined in 'other' (and is therefore implicitly
      // bound to Bottom).
      return false;
    }
    for (const auto& binding : m_map) {
      auto it = other.m_map.find(binding.first);
      if (it == other.m_map.end()) {
        // The other value is Bottom.
        return false;
      }
      if (!binding.second.leq(it->second)) {
        return false;
      }
    }
    return true;
  }

  bool equals(const HashedAbstractPartition& other) const override {
    if (m_is_top != other.m_is_top || m_map.size() != other.m_map.size()) {
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

  void join_with(const HashedAbstractPartition& other) override {
    join_like_operation(other,
                        [](Domain* x, const Domain& y) { x->join_with(y); });
  }

  void widen_with(const HashedAbstractPartition& other) override {
    join_like_operation(other,
                        [](Domain* x, const Domain& y) { x->widen_with(y); });
  }

  void meet_with(const HashedAbstractPartition& other) override {
    meet_like_operation(other,
                        [](Domain* x, const Domain& y) { x->meet_with(y); });
  }

  void narrow_with(const HashedAbstractPartition& other) override {
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
    for (const auto& other_binding : other.m_map) {
      auto binding = m_map.find(other_binding.first);
      if (binding == m_map.end()) {
        // The value is Bottom, we just insert the other value (Bottom is the
        // identity for join-like operations).
        m_map[other_binding.first] = other_binding.second;
      } else {
        // We compute the join-like combination of the values.
        operation(&binding->second, other_binding.second);
        // By construction, it's impossible to have Bottom in both operands,
        // hence the result can never be Bottom.
        RUNTIME_CHECK(!binding->second.is_bottom(), internal_error());
      }
    }
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
    for (auto it = m_map.begin(); it != m_map.end();) {
      auto other_binding = other.m_map.find(it->first);
      if (other_binding == other.m_map.end()) {
        // The other value is Bottom, we just erase the binding. We need to use
        // a different iterator, because all iterators to an erased binding are
        // invalidated.
        auto to_erase = it++;
        m_map.erase(to_erase);
      } else {
        // We compute the meet-like combination of the values.
        operation(&it->second, other_binding->second);
        if (it->second.is_bottom()) {
          // If the result is Bottom, we erase the binding.
          auto to_erase = it++;
          m_map.erase(to_erase);
        } else {
          ++it;
        }
      }
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
    if (value.is_bottom()) {
      m_map.erase(label);
    } else {
      m_map.insert_or_assign(label, std::forward<D>(value));
    }
    return *this;
  }

  std::unordered_map<Label, Domain, LabelHash, LabelEqual> m_map;
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
