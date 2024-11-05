/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WriteBarrierLoweringPass.h"

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexAsm.h"
#include "Show.h"
#include "Walkers.h"
#include <DexStructure.h>

namespace {
using namespace dex_asm;

// Lredex/$StoreFenceHelper;.DUMMY_VOLATILE:I
DexField* make_volatile_field(DexClass* cls) {
  auto name = DexString::make_string("DUMMY_VOLATILE");
  auto unsafe_field =
      DexField::make_field(cls->get_type(), name, DexType::make_type("I"))
          ->make_concrete(ACC_PUBLIC | ACC_VOLATILE | ACC_STATIC);
  cls->add_field(unsafe_field);
  unsafe_field->set_deobfuscated_name(show_deobfuscated(unsafe_field));
  return unsafe_field;
}

/**
 * We create a helper class that contains a dummy volatile field, that
 * we will use write to volatile field to construct a write barrier.
 */
DexFieldRef* materialize_write_barrier_field(DexStoresVector* stores) {
  std::string helper_cls_name = "Lredex/$StoreFenceHelper;";
  auto helper_type = DexType::get_type(helper_cls_name);
  always_assert(!helper_type);
  helper_type = DexType::make_type(helper_cls_name);
  ClassCreator cc(helper_type);
  cc.set_access(ACC_PUBLIC | ACC_FINAL);
  cc.set_super(type::java_lang_Object());
  DexClass* write_barrier_cls = cc.create();
  auto dummy_volatile_field = make_volatile_field(write_barrier_cls);

  // Put in primary dex.
  auto& dexen = (*stores)[0].get_dexen()[0];
  dexen.push_back(write_barrier_cls);
  return dummy_volatile_field;
}

} // namespace

void WriteBarrierLoweringPass::eval_pass(DexStoresVector&,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  // TODO: Handle the extra field ref for primary dex only.
  m_reserved_refs_handle = mgr.reserve_refs(name(),
                                            ReserveRefsInfo(/* frefs */ 1,
                                                            /* trefs */ 1,
                                                            /* mrefs */ 0));
}

void WriteBarrierLoweringPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles&,
                                        PassManager& mgr) {
  always_assert(m_reserved_refs_handle);
  mgr.release_reserved_refs(*m_reserved_refs_handle);
  m_reserved_refs_handle = std::nullopt;

  std::atomic<size_t> write_barrier_instructions{0};

  auto scope = build_class_scope(stores);

  InsertOnlyConcurrentSet<IRInstruction*> volatile_field_writes;

  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    auto& cfg = code.cfg();
    auto ii = InstructionIterable(cfg);
    std::unique_ptr<cfg::CFGMutation> mutation;
    size_t local_write_barrier_instructions{0};
    boost::optional<reg_t> tmp_reg = boost::none;
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto& mie = *it;
      if (!opcode::is_write_barrier(mie.insn->opcode())) {
        continue;
      }

      if (!mutation) {
        mutation = std::make_unique<cfg::CFGMutation>(cfg);
      }
      // TODO: If min-sdk >= 33 we can just call VarHandle.storeStoreFence
      IRInstruction* const_insn = new IRInstruction(OPCODE_CONST);
      if (!tmp_reg) {
        tmp_reg = cfg.allocate_temp();
      }
      const_insn->set_literal(0)->set_dest(tmp_reg.get());
      IRInstruction* sput_insn = new IRInstruction(OPCODE_SPUT);
      sput_insn->set_field(nullptr);
      sput_insn->set_srcs_size(1);
      sput_insn->set_src(0, tmp_reg.get());
      volatile_field_writes.insert(sput_insn);
      mutation->replace(it, {const_insn, sput_insn});
      ++local_write_barrier_instructions;
    }
    if (mutation) {
      mutation->flush();
      write_barrier_instructions += local_write_barrier_instructions;
    } else {
      always_assert(local_write_barrier_instructions == 0);
    }
  });

  always_assert(write_barrier_instructions == volatile_field_writes.size());

  mgr.incr_metric("added_write_barriers", (size_t)write_barrier_instructions);
  if (volatile_field_writes.empty()) {
    return;
  }
  DexFieldRef* volatile_field = materialize_write_barrier_field(&stores);
  for (auto insn : volatile_field_writes) {
    insn->set_field(volatile_field);
  }
}

static WriteBarrierLoweringPass s_pass;
