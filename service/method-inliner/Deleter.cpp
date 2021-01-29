/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Deleter.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

size_t delete_methods(
    std::vector<DexClass*>& scope,
    std::unordered_set<DexMethod*>& removable,
    ConcurrentSet<DexMethod*>& delayed_make_static,
    std::function<DexMethod*(DexMethodRef*, MethodSearch search)> resolver) {

  // if a removable candidate is invoked do not delete
  ConcurrentSet<DexMethod*> removable_to_erase;
  walk::parallel::opcodes(
      scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, IRInstruction* insn) {
        if (opcode::is_an_invoke(insn->opcode())) {
          auto callee = resolver(insn->get_method(), opcode_to_search(insn));
          if (callee != nullptr && removable.count(callee)) {
            removable_to_erase.insert(callee);
          }
        }
      });
  for (auto method : removable_to_erase) {
    removable.erase(method);
  }

  // if a removable candidate is referenced by an annotation do not delete
  walk::annotations(scope, [&](DexAnnotation* anno) {
    for (auto anno_element : anno->anno_elems()) {
      auto ev = anno_element.encoded_value;
      if (ev->evtype() == DEVT_METHOD) {
        DexEncodedValueMethod* evm = static_cast<DexEncodedValueMethod*>(ev);
        if (evm->method()->is_def()) {
          removable.erase(evm->method()->as_def());
        }
      }
    }
  });

  size_t deleted = 0;
  for (auto callee : removable) {
    if (!callee->is_concrete()) continue;
    if (!can_delete(callee)) continue;
    auto cls = type_class(callee->get_class());
    always_assert_log(cls != nullptr,
                      "%s is concrete but does not have a DexClass\n",
                      SHOW(callee));
    cls->remove_method(callee);
    delayed_make_static.erase(callee);
    DexMethod::erase_method(callee);
    deleted++;
    TRACE(DELMET, 4, "removing %s", SHOW(callee));
  }
  return deleted;
}
