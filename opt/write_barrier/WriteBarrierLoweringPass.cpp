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
          ->make_concrete(ACC_PRIVATE | ACC_VOLATILE | ACC_STATIC);
  cls->add_field(unsafe_field);
  unsafe_field->set_deobfuscated_name(show_deobfuscated(unsafe_field));
  return unsafe_field;
}

/**
 * Lredex/$StoreFenceHelper;.storeStoreFence:()V
 *   const v0 0
 *   sput v0 Lredex/$StoreFenceHelper;.DUMMY_VOLATILE:I
 */
DexMethod* make_store_fence_method(DexClass* cls,
                                   DexField* dummy_volatile_field) {
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  DexMethod* method =
      DexMethod::make_method(
          cls->get_type(), DexString::make_string("storeStoreFence"), proto)
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false);
  method->set_deobfuscated_name(show_deobfuscated(method));
  method->set_code(std::make_unique<IRCode>(method, 0));
  method->rstate.set_no_outlining();
  method->rstate.set_dont_inline();
  cls->add_method(method);
  auto* code = method->get_code();
  code->set_debug_item(std::make_unique<DexDebugItem>());
  auto artificial_pos = std::make_unique<DexPosition>(
      DexString::make_string("RedexGenerated"), 0);
  artificial_pos->bind(DexString::make_string(show_deobfuscated(method)));
  code->push_back(std::move(artificial_pos));
  code->push_back(dasm(OPCODE_CONST, {0_v, 0_L}));
  code->push_back(dasm(OPCODE_SPUT, dummy_volatile_field, {0_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(1);
  code->build_cfg();

  return method;
}

/**
 * We create a helper class that contains a dummy volatile field and a
 * storeStoreFence method, that we will use write to volatile field
 * to construct a write barrier.
 */
DexMethodRef* materialize_write_barrier_method(DexStoresVector* stores) {
  std::string helper_cls_name = "Lredex/$StoreFenceHelper;";
  auto helper_type = DexType::get_type(helper_cls_name);
  always_assert(!helper_type);
  helper_type = DexType::make_type(helper_cls_name);
  ClassCreator cc(helper_type);
  cc.set_access(ACC_PUBLIC | ACC_FINAL);
  cc.set_super(type::java_lang_Object());
  DexClass* write_barrier_cls = cc.create();
  auto dummy_volatile_field = make_volatile_field(write_barrier_cls);
  auto store_fence_method =
      make_store_fence_method(write_barrier_cls, dummy_volatile_field);

  // Put in primary dex.
  auto& dexen = (*stores)[0].get_dexen()[0];
  dexen.push_back(write_barrier_cls);
  return store_fence_method;
}

} // namespace

void WriteBarrierLoweringPass::eval_pass(DexStoresVector&,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  // TODO: Handle the extra field ref for primary dex only.
  m_reserved_refs_handle = mgr.reserve_refs(name(),
                                            ReserveRefsInfo(/* frefs */ 1,
                                                            /* trefs */ 1,
                                                            /* mrefs */ 1));
}
void WriteBarrierLoweringPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles&,
                                        PassManager& mgr) {
  always_assert(m_reserved_refs_handle);
  mgr.release_reserved_refs(*m_reserved_refs_handle);
  m_reserved_refs_handle = std::nullopt;

  std::atomic<size_t> write_barrier_instructions{0};

  auto scope = build_class_scope(stores);

  InsertOnlyConcurrentSet<IRInstruction*> store_fence_invokes;

  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    auto& cfg = code.cfg();
    auto ii = InstructionIterable(cfg);
    std::unique_ptr<cfg::CFGMutation> mutation;
    size_t local_write_barrier_instructions{0};
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto& mie = *it;
      if (!opcode::is_write_barrier(mie.insn->opcode())) {
        continue;
      }

      if (!mutation) {
        mutation = std::make_unique<cfg::CFGMutation>(cfg);
      }
      // TODO: If min-sdk >= 31 we can just call VarHandle.storeStoreFence
      // TODO: instead of method invoke, we can just insert const 0
      // and field write.
      auto store_fence_invoke =
          (new IRInstruction(OPCODE_INVOKE_STATIC))->set_method(nullptr);
      store_fence_invokes.insert(store_fence_invoke);
      mutation->replace(it, {store_fence_invoke});
      ++local_write_barrier_instructions;
    }
    if (mutation) {
      mutation->flush();
      write_barrier_instructions += local_write_barrier_instructions;
    } else {
      always_assert(local_write_barrier_instructions == 0);
    }
  });

  always_assert(write_barrier_instructions == store_fence_invokes.size());

  mgr.incr_metric("added_write_barriers", (size_t)write_barrier_instructions);
  if (store_fence_invokes.empty()) {
    return;
  }
  DexMethodRef* store_fence_method_ref =
      materialize_write_barrier_method(&stores);
  for (auto insn : store_fence_invokes) {
    insn->set_method(store_fence_method_ref);
  }
}

static WriteBarrierLoweringPass s_pass;
