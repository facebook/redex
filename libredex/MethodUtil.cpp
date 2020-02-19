/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodUtil.h"

#include "ControlFlow.h"

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

bool no_invoke_super(const DexMethod* method) {
  auto code = method->get_code();
  always_assert(code);

  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_SUPER) {
      return false;
    }
  }

  return true;
}

DexMethod* java_lang_Enum_ctor() {
  return static_cast<DexMethod*>(
      DexMethod::make_method("Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V"));
}

DexMethod* java_lang_Enum_ordinal() {
  return static_cast<DexMethod*>(
      DexMethod::make_method("Ljava/lang/Enum;.ordinal:()I"));
}

DexMethod* java_lang_Enum_name() {
  return static_cast<DexMethod*>(
      DexMethod::make_method("Ljava/lang/Enum;.name:()Ljava/lang/String;"));
}

DexMethod* java_lang_Enum_equals() {
  return static_cast<DexMethod*>(
      DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z"));
}

DexMethod* java_lang_Integer_valueOf() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;"));
}

DexMethod* java_lang_Integer_intValue() {
  return static_cast<DexMethod*>(
      DexMethod::make_method("Ljava/lang/Integer;.intValue:()I"));
}

}; // namespace method
