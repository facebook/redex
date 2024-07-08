/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <json/value.h>
#include <set>

#include "ConfigFiles.h"
#include "ConstantPropagationAnalysis.h"
#include "ResourcesInliningPass.h"
#include "Trace.h"
#include "Walkers.h"

void ResourcesInliningPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  std::string zip_dir;
  conf.get_json_config().get("apk_dir", "", zip_dir);
  always_assert(!zip_dir.empty());
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();
  auto inlinable_resources = res_table->get_inlinable_resource_values();

  const Scope scope = build_class_scope(stores);

  ResourcesInliningPass::optimization_pass(scope, inlinable_resources);
}

MethodTransformsMap ResourcesInliningPass::optimization_pass(
    const Scope& scope,
    const std::unordered_map<uint32_t, resources::InlinableValue>&
        inlinable_resources) {
  std::unordered_set<DexMethodRef*> dex_method_refs = {
      DexMethod::get_method("Landroid/content/res/Resources;.getBoolean:(I)Z"),
      DexMethod::get_method("Landroid/content/res/Resources;.getColor:(I)I"),
      DexMethod::get_method("Landroid/content/res/Resources;.getInteger:(I)I"),
      DexMethod::get_method(
          "Landroid/content/res/Resources;.getString:(I)Ljava/lang/String;")};

  MethodTransformsMap possible_transformations;

  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (method->rstate.no_optimizations()) {
      return;
    }

    namespace cp = constant_propagation;
    using CombinedAnalyzer =
        InstructionAnalyzerCombiner<cp::StaticFinalFieldAnalyzer,
                                    cp::HeapEscapeAnalyzer,
                                    cp::PrimitiveAnalyzer>;

    auto get_code = method->get_code();
    if (get_code == nullptr) {
      return;
    }
    auto& cfg = get_code->cfg();

    cp::intraprocedural::FixpointIterator intra_cp(
        cfg, CombinedAnalyzer(nullptr, nullptr, nullptr));
    intra_cp.run(ConstantEnvironment());

    std::vector<InlinableOptimization> transforms;
    for (auto* block : cfg.blocks()) {
      auto env = intra_cp.get_entry_state_at(block);
      auto last_insn = block->get_last_insn();
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
            (dex_method_refs.find(insn->get_method()) !=
             dex_method_refs.end())) {
          auto field_domain = env.get<SignedConstantDomain>(insn->src(1));
          auto const_value = field_domain.get_constant();
          if (const_value != boost::none &&
              inlinable_resources.find(const_value.value()) !=
                  inlinable_resources.end()) {
            auto insertable = InlinableOptimization();
            insertable.insn = insn;
            insertable.inlinable_value =
                inlinable_resources.at(const_value.value());
            transforms.push_back(insertable);
          }
        }
        intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
      }
    }
    if (!transforms.empty()) {
      possible_transformations.emplace(method, std::move(transforms));
    }
  });
  return possible_transformations;
}

static ResourcesInliningPass s_pass;
