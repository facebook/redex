/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodUtil.h"

#include "ControlFlow.h"
#include "EditableCfgAdapter.h"
#include "TypeUtil.h"

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

bool clinit_may_have_side_effects(const DexClass* cls) {
  auto clinit = cls->get_clinit();
  if (clinit && clinit->get_code()) {
    bool non_trivial{false};
    editable_cfg_adapter::iterate_with_iterator(
        clinit->get_code(), [&non_trivial, cls](const IRList::iterator& it) {
          auto insn = it->insn;
          if (opcode::is_an_invoke(insn->opcode()) ||
              insn->opcode() == OPCODE_NEW_INSTANCE ||
              (insn->has_field() &&
               insn->get_field()->get_class() != cls->get_type())) {
            non_trivial = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          return editable_cfg_adapter::LOOP_CONTINUE;
        });
    if (non_trivial) {
      return true;
    }
  }
  if (cls->get_super_class() == type::java_lang_Object()) {
    return false;
  }
  auto super_cls = type_class(cls->get_super_class());
  return !super_cls || clinit_may_have_side_effects(super_cls);
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

DexMethod* kotlin_jvm_internal_Intrinsics_checkParameterIsNotNull() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/"
      "Object;Ljava/lang/String;)V"));
}

DexMethod* kotlin_jvm_internal_Intrinsics_checkNotNullParameter() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Lkotlin/jvm/internal/Intrinsics;.checkNotNullParameter:(Ljava/lang/"
      "Object;Ljava/lang/String;)V"));
}

DexMethod* kotlin_jvm_internal_Intrinsics_checExpressionValueIsNotNull() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/"
      "lang/Object;Ljava/lang/String;)V"));
}

DexMethod* kotlin_jvm_internal_Intrinsics_checkNotNullExpressionValue() {
  return static_cast<DexMethod*>(DexMethod::make_method(
      "Lkotlin/jvm/internal/Intrinsics;.checkNotNullExpressionValue:(Ljava/"
      "lang/Object;Ljava/lang/String;)V"));
}
}; // namespace method
