/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Match.h"

namespace m {
namespace detail {

bool is_assignable_to_interface(const DexType* type, const DexType* iface) {
  if (type == iface) return true;
  auto cls = type_class(type);
  if (cls) {
    for (auto extends : *cls->get_interfaces()) {
      if (is_assignable_to_interface(extends, iface)) {
        return true;
      }
    }
  }
  return false;
}

bool is_assignable_to(const DexType* child, const DexType* parent) {
  // Check class hierarchy
  auto super = child;
  while (super != nullptr) {
    if (parent == super) return true;
    const auto cls = type_class(super);
    if (cls == nullptr) break;
    super = cls->get_super_class();
  }
  // Check interface hierarchy
  DexClass* parent_cls = type_class(parent);
  return parent_cls && is_interface(parent_cls) &&
         is_assignable_to_interface(child, parent);
}

bool is_default_constructor(const DexMethod* meth) {
  if (!is_static(meth) && method::is_constructor(meth) &&
      method::has_no_args(meth) && method::has_code(meth)) {
    auto ii = InstructionIterable(meth->get_code());
    auto it = ii.begin();
    const auto end = ii.end();

    return it != end && it++->insn->opcode() == OPCODE_INVOKE_DIRECT &&
           it != end && it++->insn->opcode() == OPCODE_RETURN_VOID && it == end;
  }
  return false;
}

} // namespace detail
} // namespace m
