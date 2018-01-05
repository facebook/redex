/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Deleter.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Walkers.h"

size_t delete_methods(
    std::vector<DexClass*>& scope, std::unordered_set<DexMethod*>& removable,
    std::function<DexMethod*(DexMethodRef*, MethodSearch search)> resolver) {

  // if a removable candidate is invoked do not delete
  walk::opcodes(scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, IRInstruction* insn) {
        if (is_invoke(insn->opcode())) {
          auto callee = resolver(insn->get_method(), opcode_to_search(insn));
          if (callee != nullptr) {
            removable.erase(callee);
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
    if (callee->is_virtual()) {
      cls->remove_method(callee);
    } else {
      cls->remove_method(callee);
    }
    deleted++;
    TRACE(DELMET, 4, "removing %s\n", SHOW(callee));
  }
  return deleted;
}
