/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodUtil.h"

#include "ControlFlow.h"
#include "DexClass.h"

namespace method {

bool is_init(const DexMethodRef* method) {
  return strcmp(method->get_name()->c_str(), "<init>") == 0;
}

bool is_clinit(const DexMethodRef* method) {
  return strcmp(method->get_name()->c_str(), "<clinit>") == 0;
}

bool is_trivial_clinit(const DexMethod* method) {
  always_assert(is_clinit(method));
  auto ii = InstructionIterable(method->get_code());
  return std::none_of(ii.begin(), ii.end(), [](const MethodItemEntry& mie) {
    return mie.insn->opcode() != OPCODE_RETURN_VOID;
  });
}

}; // namespace method
