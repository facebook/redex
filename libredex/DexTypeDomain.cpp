/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexTypeDomain.h"

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

sparta::AbstractValueKind DexTypeValue::join_with(const DexTypeValue& other) {
  if (equals(other)) {
    return sparta::AbstractValueKind::Value;
  }

  const DexClass* this_cls =
      const_cast<const DexClass*>(type_class(get_dex_type()));
  const DexClass* other_cls =
      const_cast<const DexClass*>(type_class(other.get_dex_type()));
  // External classes. Cannot perform subclass check.
  if (!this_cls || !other_cls) {
    m_dex_type = nullptr;
    return sparta::AbstractValueKind::Top;
  }

  // Direct subclass relation.
  if (type::is_subclass(get_dex_type(), other.get_dex_type())) {
    if (!are_interfaces_mergeable_to(other_cls, this_cls)) {
      m_dex_type = nullptr;
      return sparta::AbstractValueKind::Top;
    }
    return sparta::AbstractValueKind::Value;
  } else if (type::is_subclass(other.get_dex_type(), get_dex_type())) {
    if (!are_interfaces_mergeable_to(this_cls, other_cls)) {
      m_dex_type = nullptr;
      return sparta::AbstractValueKind::Top;
    }
    m_dex_type = other.get_dex_type();
    return sparta::AbstractValueKind::Value;
  }

  // Share common base type simple scenario.
  auto this_super = this_cls->get_super_class();
  auto other_super = other_cls->get_super_class();
  auto this_super_cls = type_class(this_super);
  auto other_super_cls = type_class(other_super);
  if (!this_super_cls || !other_super_cls) {
    m_dex_type = nullptr;
    return sparta::AbstractValueKind::Top;
  }
  if (this_super && type::is_subclass(this_super, other.get_dex_type())) {
    if (!are_interfaces_mergeable_to(other_cls, this_super_cls)) {
      m_dex_type = nullptr;
      return sparta::AbstractValueKind::Top;
    }
    m_dex_type = this_super;
    return sparta::AbstractValueKind::Value;
  } else if (other_super && type::is_subclass(other_super, get_dex_type())) {
    if (!are_interfaces_mergeable_to(this_cls, other_super_cls)) {
      m_dex_type = nullptr;
      return sparta::AbstractValueKind::Top;
    }
    m_dex_type = other_super;
    return sparta::AbstractValueKind::Value;
  }

  // Give up. Rewrite to top.
  m_dex_type = nullptr;
  return sparta::AbstractValueKind::Top;
}

} // namespace  dtv_impl

std::ostream& operator<<(std::ostream& output, const DexType* dex_type) {
  output << show(dex_type);
  return output;
}
