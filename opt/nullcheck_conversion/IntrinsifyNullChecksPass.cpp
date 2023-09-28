/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IntrinsifyNullChecksPass.h"

#include "ControlFlow.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "Show.h"
#include "Walkers.h"

using namespace dex_asm;

void IntrinsifyNullChecksPass::create_null_check_class(
    DexStoresVector* stores) {
  std::string name = "Lredex/$NullCheck;";
  DexType* type = DexType::get_type(name);
  while (type) {
    name.insert(name.size() - 1, "$u");
    type = DexType::get_type(name);
  }
  type = DexType::make_type(name);
  ClassCreator cc(type);
  cc.set_access(ACC_PUBLIC | ACC_FINAL);
  cc.set_super(type::java_lang_Object());
  DexClass* cls = cc.create();
  cls->rstate.set_generated();
  cls->rstate.set_clinit_has_no_side_effects();
  cls->rstate.set_name_used();
  cls->rstate.set_dont_rename();
  // Crate method for null check.
  auto meth_name = DexString::make_string("null_check");
  DexProto* proto = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::java_lang_Object()}));
  DexMethod* method = DexMethod::make_method(cls->get_type(), meth_name, proto)
                          ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_deobfuscated_name(show_deobfuscated(method));
  // redex.NullCheck.null_check:(Ljava/lang/Object;)V
  //   if-eqz v0, 0003 // +0003
  //   return
  //   new-instance Ljava/lang/NullPointerException;
  //   move-result-pseudo-object v0
  //   invoke-direct {v0}, Ljava/lang/NullPointerException;.<init>:()V
  //   throw v0
  method->set_code(std::make_unique<IRCode>(method, 0));
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();
  auto entry_block = cfg.entry_block();
  auto throw_block = cfg.create_block();
  throw_block->push_back({dasm(OPCODE_NEW_INSTANCE, m_NPE_type, {}),
                          dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
                          dasm(OPCODE_INVOKE_DIRECT, m_NPE_ref, {0_v}),
                          dasm(OPCODE_THROW, {0_v})});
  auto return_block = cfg.create_block();
  return_block->push_back(dasm(OPCODE_RETURN_VOID));
  cfg.create_branch(entry_block, dasm(OPCODE_IF_EQZ, {0_v}), return_block,
                    throw_block);
  cfg.recompute_registers_size();
  method->rstate.set_keepnames(keep_reason::KeepReasonType::UNKNOWN);
  method->rstate.set_dont_inline();
  cls->add_method(method);
  TRACE(NCI, 1, "the added method is %s\n", SHOW(method));
  TRACE(NCI, 1, "the code is %s\n", SHOW(cfg));
  auto& dexen = (*stores)[0].get_dexen()[0];
  dexen.push_back(cls);
}

void IntrinsifyNullChecksPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles&,
                                        PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  m_getClass_ref =
      DexMethod::get_method("Ljava/lang/Object;.getClass:()Ljava/lang/Class;");
  m_NPE_ref =
      DexMethod::make_method("Ljava/lang/NullPointerException;.<init>:()V");
  m_NPE_type = DexType::make_type("Ljava/lang/NullPointerException;");

  if (m_getClass_ref == nullptr) {
    // Could not find Ljava/lang/Object;.getClass:()Ljava/lang/Class;, just
    // return;
    return;
  }

  // Create a null-check class in primiary dex.
  create_null_check_class(&stores);
  m_null_check_ref = DexMethod::get_method(
      "Lredex/$NullCheck;.null_check:(Ljava/lang/Object;)V");
  always_assert(m_null_check_ref);
  m_stats = walk::parallel::methods<Stats>(
      scope, [&](DexMethod* method) { return convert_getClass(method); });
  m_stats.report(mgr);
}

IntrinsifyNullChecksPass::Stats IntrinsifyNullChecksPass::convert_getClass(
    DexMethod* method) {
  Stats stats;
  IRCode* code = method->get_code();
  if (code == nullptr) {
    return stats;
  }
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (!opcode::is_invoke_virtual(insn->opcode())) {
        continue;
      }
      auto mref = insn->get_method();
      if (mref != m_getClass_ref) {
        continue;
      }
      // Found invoke-virtual objects.getClass().
      stats.num_of_obj_getClass++;

      auto cfg_it = block->to_cfg_instruction_iterator(mie);
      auto move_result = cfg.move_result_of(cfg_it);
      if (!move_result.is_end()) {
        // If there is a move-result followed by, this obj is used later, no
        // need to add an explicitly null check.
        continue;
      }
      // Replace this getClass() with an explicit null check call after that.
      stats.num_of_convt_getClass++;
      TRACE(NCI, 1, "replace getClass with null-check call %s\n",
            SHOW(method->get_name()));
      insn->set_opcode(OPCODE_INVOKE_STATIC);
      insn->set_method(m_null_check_ref);
    }
  }
  return stats;
}

static IntrinsifyNullChecksPass s_pass;
