/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodFixup.h"

#include "DexOpcode.h"
#include "IRCode.h"
#include "Show.h"
#include "Walkers.h"

namespace method_fixup {

void fixup_references_to_removed_methods(
    const Scope& scope,
    UnorderedMap<DexMethodRef*, DexMethodRef*>& removed_vmethods) {
  // Forward chains.
  auto&& ui = UnorderedIterable(removed_vmethods);
  using iterator = std::remove_reference_t<decltype(ui)>::iterator;
  std::function<DexMethodRef*(iterator&)> forward;
  forward = [&forward, &ui](iterator& it) {
    auto it2 = ui.find(it->second);
    if (it2 != ui.end()) {
      it->second = forward(it2);
    }
    return it->second;
  };
  for (auto it = ui.begin(); it != ui.end(); it++) {
    forward(it);
  }

  // Fixup references in code to deleted vmathods to point to the base one.
  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    editable_cfg_adapter::iterate(&code, [&](MethodItemEntry& mie) {
      auto insn = mie.insn;
      auto op = insn->opcode();
      if (opcode::is_invoke_virtual(op)) {
        auto it = removed_vmethods.find(insn->get_method());
        if (it != removed_vmethods.end()) {
          insn->set_method(it->second);
        }
      }
      always_assert_log(!opcode::is_invoke_virtual(op) ||
                            !opcode::is_invoke_interface(op) ||
                            !removed_vmethods.count(insn->get_method()),
                        "%s", SHOW(insn));
      return editable_cfg_adapter::LOOP_CONTINUE;
    });
  });
}

} // namespace method_fixup
