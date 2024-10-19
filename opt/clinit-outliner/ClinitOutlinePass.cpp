/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClinitOutlinePass.h"

#include "ApiLevelChecker.h"
#include "BaselineProfile.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

void ClinitOutlinePass::bind_config() {
  bind("min_clinit_size", 16, m_min_clinit_size);
  trait(Traits::Pass::unique, true);
}

void ClinitOutlinePass::run_pass(DexStoresVector& stores,
                                 ConfigFiles& conf,
                                 PassManager& mgr) {
  auto& method_profiles = conf.get_method_profiles();
  auto baseline_profile = baseline_profiles::get_baseline_profile(
      conf.get_baseline_profile_config(), method_profiles);

  auto scope = build_class_scope(stores);
  std::atomic<size_t> affected_final_fields{0};
  InsertOnlyConcurrentMap<DexMethod*, DexMethod*> outlined_clinits;

  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (!method::is_clinit(method) || method->rstate.no_optimizations() ||
        method->rstate.should_not_outline()) {
      return;
    }

    auto it = baseline_profile.methods.find(method);
    if (it == baseline_profile.methods.end()) {
      return;
    }
    if (!it->second.hot) {
      return;
    }

    if (code.estimate_code_units() < m_min_clinit_size) {
      // Probably not worth the overhead of another method.
      return;
    }

    std::unordered_set<DexField*> final_fields;
    for (auto& mie : InstructionIterable(code.cfg())) {
      auto insn = mie.insn;
      if (opcode::is_an_sput(insn->opcode())) {
        auto field = insn->get_field();
        auto resolved_field = resolve_field(field, FieldSearch::Static);
        if (!resolved_field || !is_final(resolved_field)) {
          continue;
        }
        always_assert(field->get_class() == method->get_class());
        if (final_fields.insert(resolved_field).second) {
          resolved_field->set_access(resolved_field->get_access() & ~ACC_FINAL);
        }
      }
    }
    affected_final_fields.fetch_add(final_fields.size());
    auto outlined_clinit =
        DexMethod::make_method(method->get_class(),
                               DexString::make_string("clinit$outlined"),
                               method->get_proto())
            ->make_concrete(ACC_STATIC | ACC_PUBLIC, method->release_code(),
                            /* is_virtual */ false);

    outlined_clinit->rstate.set_generated();
    outlined_clinit->rstate.set_dont_inline();
    int api_level = api::LevelChecker::get_method_level(method);
    outlined_clinit->rstate.set_api_level(api_level);
    outlined_clinit->set_deobfuscated_name(show_deobfuscated(outlined_clinit));

    method->set_code(std::make_unique<IRCode>());
    auto new_code = method->get_code();
    auto sb = source_blocks::get_first_source_block_of_method(outlined_clinit);
    if (sb) {
      auto new_sb = std::make_unique<SourceBlock>(*sb);
      new_sb->id = SourceBlock::kSyntheticId;
      new_code->push_back(std::move(new_sb));
    }
    new_code->push_back(
        (new IRInstruction(OPCODE_INVOKE_STATIC))->set_method(outlined_clinit));
    new_code->push_back(new IRInstruction(OPCODE_RETURN_VOID));
    new_code->set_registers_size(0);
    new_code->build_cfg();

    outlined_clinits.emplace(method, outlined_clinit);
  });

  for (auto [method, outlined_clinit] : outlined_clinits) {
    type_class(method->get_class())->add_method(outlined_clinit);
    method_profiles.derive_stats(outlined_clinit, {method});
  }

  mgr.set_metric("affected_clinits", outlined_clinits.size());
  mgr.set_metric("affected_final_fields", affected_final_fields.load());
  TRACE(CLINIT_OUTLINE, 1, "affected clinits: %zu, affected fields: %zu",
        outlined_clinits.size(), affected_final_fields.load());
}

static ClinitOutlinePass s_pass;
