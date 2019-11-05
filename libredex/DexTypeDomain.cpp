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

sparta::AbstractValueKind DexTypeValue::join_with(const DexTypeValue& other) {
  if (equals(other)) {
    return sparta::AbstractValueKind::Value;
  }

  // External classes. Cannot perform subclass check.
  const DexClass* this_cls =
      const_cast<const DexClass*>(type_class(get_dex_type()));
  const DexClass* other_cls =
      const_cast<const DexClass*>(type_class(other.get_dex_type()));
  if (!this_cls || !other_cls) {
    m_dex_type = type::java_lang_Object();
    return sparta::AbstractValueKind::Value;
  }
  // Direct subclass relation.
  if (is_subclass(get_dex_type(), other.get_dex_type())) {
    return sparta::AbstractValueKind::Value;
  } else if (is_subclass(other.get_dex_type(), get_dex_type())) {
    m_dex_type = other.get_dex_type();
    return sparta::AbstractValueKind::Value;
  }
  // Share common base type simple scenario.
  auto this_super_cls = this_cls->get_super_class();
  auto other_super_cls = other_cls->get_super_class();
  if (this_super_cls && is_subclass(this_super_cls, other.get_dex_type())) {
    m_dex_type = this_super_cls;
    return sparta::AbstractValueKind::Value;
  } else if (other_super_cls && is_subclass(other_super_cls, get_dex_type())) {
    m_dex_type = other_super_cls;
    return sparta::AbstractValueKind::Value;
  }

  // Give up. Rewrite to java.lang.Object.
  m_dex_type = type::java_lang_Object();
  return sparta::AbstractValueKind::Value;
}

} // namespace  dtv_impl

std::ostream& operator<<(std::ostream& output, const DexType* dex_type) {
  output << show(dex_type);
  return output;
}
