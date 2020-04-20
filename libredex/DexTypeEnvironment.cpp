/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexTypeEnvironment.h"

#include <boost/optional/optional_io.hpp>
#include <ostream>

namespace dtv_impl {

// Is `left` a subset of `right`
bool is_subset(DexTypeList* left, DexTypeList* right) {
  std::unordered_set<DexType*> rset(right->begin(), right->end());
  for (auto ltype : *left) {
    if (rset.count(ltype) == 0) {
      return false;
    }
  }
  return true;
}

// Can the interface identity of `left` be merged into `right`.
bool are_interfaces_mergeable_to(const DexClass* left, const DexClass* right) {
  always_assert(left && right);
  if (left->get_interfaces()->size() == 0) {
    return true;
  }
  return is_subset(left->get_interfaces(), right->get_interfaces());
}

/*
 * Try to find type on `l`'s parent chain that is also a parent of `r`.
 */
const DexType* find_common_parent(const DexType* l, const DexType* r) {
  always_assert(l && r);
  if (l == r) {
    return l;
  }
  auto parent = l;
  while (parent) {
    if (type::is_subclass(parent, r)) {
      return parent;
    }
    auto parent_cls = type_class(parent);
    if (!parent_cls) {
      break;
    }
    parent = parent_cls->get_super_class();
  }
  return nullptr;
}

sparta::AbstractValueKind DexTypeValue::join_with(const DexTypeValue& other) {
  if (equals(other)) {
    return kind();
  }
  if (is_none()) {
    m_dex_type = other.get_dex_type();
    return sparta::AbstractValueKind::Value;
  } else if (other.is_none()) {
    return sparta::AbstractValueKind::Value;
  }

  const DexClass* this_cls =
      const_cast<const DexClass*>(type_class(get_dex_type()));
  const DexClass* other_cls =
      const_cast<const DexClass*>(type_class(other.get_dex_type()));
  // External classes/missing class definition. Fall back to top.
  if (!this_cls || !other_cls) {
    clear();
    return sparta::AbstractValueKind::Top;
  }

  auto parent = find_common_parent(get_dex_type(), other.get_dex_type());
  auto parent_cls = type_class(parent);
  if (parent && parent_cls) {
    if (are_interfaces_mergeable_to(this_cls, parent_cls) &&
        are_interfaces_mergeable_to(other_cls, parent_cls)) {
      m_dex_type = parent;
      return sparta::AbstractValueKind::Value;
    }
  }

  // Give up. Rewrite to top.
  clear();
  return sparta::AbstractValueKind::Top;
}

} // namespace  dtv_impl

std::ostream& operator<<(std::ostream& output, const DexType* dex_type) {
  output << show(dex_type);
  return output;
}

bool SmallSetDexTypeDomain::leq(const SmallSetDexTypeDomain& other) const {
  if (is_bottom()) {
    return true;
  }
  if (other.is_bottom()) {
    return false;
  }
  if (other.is_top()) {
    return true;
  }
  if (is_top()) {
    return false;
  }
  return m_types.is_subset_of(other.m_types);
}

bool SmallSetDexTypeDomain::equals(const SmallSetDexTypeDomain& other) const {
  if (is_bottom()) {
    return other.is_bottom();
  }
  if (is_top()) {
    return other.is_top();
  }
  return m_types.equals(other.m_types);
}

void SmallSetDexTypeDomain::join_with(const SmallSetDexTypeDomain& other) {
  if (is_top() || other.is_bottom()) {
    return;
  }
  if (other.is_top()) {
    set_to_top();
    return;
  }
  if (is_bottom()) {
    m_kind = other.m_kind;
    m_types = other.m_types;
    return;
  }
  m_types.union_with(other.m_types);
  if (m_types.size() > MAX_SET_SIZE) {
    set_to_top();
  }
}

std::ostream& operator<<(std::ostream& out, const SmallSetDexTypeDomain& x) {
  using namespace sparta;
  switch (x.kind()) {
  case sparta::AbstractValueKind::Bottom: {
    out << "_|_";
    break;
  }
  case sparta::AbstractValueKind::Top: {
    out << "T";
    break;
  }
  case sparta::AbstractValueKind::Value: {
    out << x.get_types();
    break;
  }
  }
  return out;
}

void DexTypeDomain::join_with(const DexTypeDomain& other) {
  sparta::ReducedProductAbstractDomain<DexTypeDomain,
                                       NullnessDomain,
                                       SingletonDexTypeDomain,
                                       SmallSetDexTypeDomain>::join_with(other);
  if (get<1>().is_top()) {
    apply<2>([](SmallSetDexTypeDomain* domain) { domain->set_to_top(); });
  }
}
