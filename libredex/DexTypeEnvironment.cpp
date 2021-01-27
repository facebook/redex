/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexTypeEnvironment.h"

#include <boost/optional/optional_io.hpp>
#include <ostream>

#include "Show.h"

namespace dtv_impl {

bool implements(const DexClass* cls, const DexType* intf) {
  if (is_interface(cls)) {
    return false;
  }
  for (const auto interface : cls->get_interfaces()->get_type_list()) {
    if (interface == intf) {
      return true;
    }
  }
  return false;
}

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
const DexType* find_common_super_class(const DexType* l, const DexType* r) {
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

const DexType* find_common_type(const DexType* l, const DexType* r) {
  const DexClass* l_cls = const_cast<const DexClass*>(type_class(l));
  const DexClass* r_cls = const_cast<const DexClass*>(type_class(r));
  if (!l_cls || !r_cls) {
    return nullptr;
  }

  // One is interface, and the other implements it.
  if (is_interface(l_cls) && implements(r_cls, l)) {
    return l;
  }
  if (is_interface(r_cls) && implements(l_cls, r)) {
    return r;
  }

  auto parent = find_common_super_class(l, r);
  auto parent_cls = type_class(parent);
  if (parent && parent_cls) {
    if (are_interfaces_mergeable_to(l_cls, parent_cls) &&
        are_interfaces_mergeable_to(r_cls, parent_cls)) {
      return parent;
    }
  }
  return nullptr;
}

/*
 * Only covers the simple cases here:
 * 1. Reference type arrays with the same depth level => common type array with
 * the same level
 * 2. If there's primitive array or the levels don't match => Top.
 */
const DexType* find_common_array_type(const DexType* l, const DexType* r) {
  uint32_t l_dim = type::get_array_level(l);
  uint32_t r_dim = type::get_array_level(r);

  bool has_primitive = false;
  auto l_elem_type = type::get_array_element_type(l);
  auto r_elem_type = type::get_array_element_type(r);
  if (type::is_primitive(l_elem_type) || type::is_primitive(r_elem_type)) {
    has_primitive = true;
  }
  if (!has_primitive && l_dim == r_dim) {
    auto common_element_type = find_common_type(l_elem_type, r_elem_type);
    return common_element_type
               ? type::make_array_type(common_element_type, l_dim)
               : nullptr;
  }

  return nullptr;
}

/*
 * Partially mimicing the Dalvik bytecode structural verifier:
 * https://android.googlesource.com/platform/dalvik/+/android-cts-4.4_r4/vm/analysis/CodeVerify.cpp#2462
 */
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

  auto l = get_dex_type();
  auto r = other.get_dex_type();
  if (type::is_array(l) && type::is_array(r)) {
    auto common_array_type = find_common_array_type(l, r);
    if (common_array_type) {
      m_dex_type = common_array_type;
      return sparta::AbstractValueKind::Value;
    }
  } else {
    auto common_type = find_common_type(l, r);
    if (common_type) {
      m_dex_type = common_type;
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

std::ostream& operator<<(std::ostream& output, bool val) {
  output << (val ? "true" : "false");
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

void SmallSetDexTypeDomain::widen_with(const SmallSetDexTypeDomain& other) {
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
  if (m_types.size() + other.m_types.size() > MAX_SET_SIZE) {
    set_to_top();
    return;
  }
  join_with(other);
}

std::ostream& operator<<(std::ostream& out, const SingletonDexTypeDomain& x) {
  using namespace sparta;
  switch (x.kind()) {
  case AbstractValueKind::Bottom: {
    out << "_|_";
    break;
  }
  case AbstractValueKind::Top: {
    out << "T";
    break;
  }
  case AbstractValueKind::Value: {
    auto type = x.get_dex_type();
    out << (type ? show(*type) : std::string("<NONE>"));
    break;
  }
  }
  return out;
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
