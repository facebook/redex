/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Nopper.h"

#include "CFGMutation.h"
#include "Creators.h"
#include "Show.h"
#include "StlUtil.h"

namespace {
bool can_insert_before(IROpcode opcode) {
  return !opcode::is_a_load_param(opcode) &&
         !opcode::is_move_result_any(opcode) &&
         !opcode::is_move_exception(opcode);
}
}; // namespace

namespace nopper_impl {

AuxiliaryDefs create_auxiliary_defs(DexType* nopper_type) {
  AuxiliaryDefs res;
  ClassCreator cc(nopper_type);
  cc.set_access(ACC_PUBLIC | ACC_ABSTRACT);
  cc.set_super(type::java_lang_Object());
  res.cls = cc.create();
  res.cls->rstate.set_generated();

  res.int_field = DexField::make_field(res.cls->get_type(),
                                       DexString::make_string("int_field"),
                                       type::_int())
                      ->make_concrete(ACC_STATIC | ACC_PUBLIC);
  res.int_field->set_deobfuscated_name(show_deobfuscated(res.int_field));

  auto void_void_proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));

  auto int_int_proto = DexProto::make_proto(
      type::_int(), DexTypeList::make_type_list({type::_int()}));

  res.fib_method = [&]() {
    MethodCreator mc{res.cls->get_type(), DexString::make_string("fib"),
                     int_int_proto, ACC_PUBLIC | ACC_STATIC};
    MethodBlock* block = mc.get_main_block();
    auto arg = mc.get_reg_args().front();
    auto v = mc.make_local(type::_int());
    block->load_const(v, 1);
    auto termination_block = block->if_else_test(OPCODE_IF_GT, arg, v, &block);
    termination_block->ret(arg);

    block->binop_lit(OPCODE_ADD_INT_LIT, v, v, -1);
    block->invoke(OPCODE_INVOKE_STATIC, mc.get_method(), {v});
    auto w = mc.make_local(type::_int());
    block->move_result(w, type::_int());
    block->binop_lit(OPCODE_ADD_INT_LIT, v, v, -1);
    block->invoke(OPCODE_INVOKE_STATIC, mc.get_method(), {v});
    block->move_result(v, type::_int());
    block->binop(OPCODE_ADD_INT, v, v, w);
    block->ret(v);

    return mc.create();
  }();
  res.fib_method->rstate.set_generated();
  DexAnnotationSet anno_set;
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      type::dalvik_annotation_optimization_NeverInline(),
      DexAnnotationVisibility::DAV_BUILD));
  auto access = res.fib_method->get_access();
  // attach_annotation_set requires the method to be synthetic.
  // A bit bizarre, and suggests that Redex' code to mutate annotations is
  // ripe for an overhaul. But I won't fight that here.
  res.fib_method->set_access(access | ACC_SYNTHETIC);
  (void)res.fib_method->attach_annotation_set(
      std::make_unique<DexAnnotationSet>(anno_set));
  res.fib_method->set_access(access);
  res.fib_method->get_code()->build_cfg();

  res.clinit = [&]() {
    MethodCreator mc{res.cls->get_type(), DexString::make_string("<clinit>"),
                     void_void_proto, ACC_CONSTRUCTOR | ACC_STATIC};
    MethodBlock* block = mc.get_main_block();
    auto v = mc.make_local(type::_int());
    block->load_const(v, 10);
    block->invoke(OPCODE_INVOKE_STATIC, res.fib_method, {v});
    block->move_result(v, type::_int());
    block->sput(res.int_field, v);
    block->ret_void();
    return mc.create();
  }();
  res.clinit->get_code()->build_cfg();

  return res;
}

std::vector<cfg::Block*> get_noppable_blocks(cfg::ControlFlowGraph& cfg) {
  auto blocks = cfg.blocks();
  std20::erase_if(blocks, [&cfg](auto* block) {
    if (cfg.get_succ_edge_of_type(block, cfg::EDGE_THROW) != nullptr) {
      // We don't want to deal with the complication of inserting throwing
      // instructions into blocks with throw handlers.
      return true;
    }
    auto ii = InstructionIterable(block);
    auto it = ii.begin();
    while (it != ii.end() && !can_insert_before(it->insn->opcode())) {
      it++;
    }
    return it == ii.end();
  });
  return blocks;
}

size_t insert_nops(cfg::ControlFlowGraph& cfg,
                   const std::unordered_set<cfg::Block*>& blocks,
                   AuxiliaryDefs* auxiliary_defs) {
  cfg::CFGMutation mutation(cfg);
  size_t nops_inserted = 0;
  for (auto* block : cfg.blocks()) {
    if (!blocks.count(block)) {
      continue;
    }
    auto ii = InstructionIterable(block);
    auto it = ii.begin();
    while (it != ii.end() && !can_insert_before(it->insn->opcode())) {
      it++;
    }
    always_assert(it != ii.end());
    const auto& cfg_it = block->to_cfg_instruction_iterator(it);
    std::vector<IRInstruction*> insns;
    if (auxiliary_defs != nullptr) {
      auto tmp = cfg.allocate_temp();
      insns.push_back(
          (new IRInstruction(OPCODE_CONST))->set_dest(tmp)->set_literal(4));
      insns.push_back((new IRInstruction(OPCODE_INVOKE_STATIC))
                          ->set_method(auxiliary_defs->fib_method)
                          ->set_srcs_size(1)
                          ->set_src(0, tmp));
      insns.push_back((new IRInstruction(OPCODE_MOVE_RESULT))->set_dest(tmp));
      // add some more instructions that cannot get optimized away to increase
      // code size
      for (size_t iterations = 0; iterations < 4; iterations++) {
        insns.push_back((new IRInstruction(OPCODE_ADD_INT_LIT))
                            ->set_dest(tmp)
                            ->set_src(0, tmp)
                            ->set_literal(27));
        insns.push_back((new IRInstruction(OPCODE_MUL_INT_LIT))
                            ->set_dest(tmp)
                            ->set_src(0, tmp)
                            ->set_literal(77));
      }
      insns.push_back((new IRInstruction(OPCODE_SPUT))
                          ->set_field(auxiliary_defs->int_field)
                          ->set_src(0, tmp));
    } else {
      insns.push_back(new IRInstruction(OPCODE_NOP));
    }
    mutation.insert_before(cfg_it, std::move(insns));
    nops_inserted++;
  }
  mutation.flush();
  return nops_inserted;
}

} // namespace nopper_impl
