/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObjectSensitiveDcePass.h"

#include <fstream>
#include <functional>

#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "HierarchyUtil.h"
#include "InitClassPruner.h"
#include "InitClassesWithSideEffects.h"
#include "LocalPointersAnalysis.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "SummarySerialization.h"
#include "Transform.h"
#include "Walkers.h"

/*
 * This pass tries to identify writes to registers and objects that never get
 * read from. Modeling dead object field writes is particularly useful in
 * conjunction with RemoveUnusedFieldsPass. Suppose we have an unused field
 * Foo.x:
 *
 *   new-instance v0 LFoo;
 *   invoke-direct {v0} LFoo;.<init>()V
 *   sput-object v0 LBar;.x:LFoo; # RMUF will remove this
 *
 * If we can determine that Foo's constructor does not modify anything
 * outside of its `this` argument, we will be able to remove the invoke-direct
 * call as well as the new-instance instruction.
 *
 * In contrast, LocalDce can only identify unused writes to registers -- it
 * knows nothing about objects. The trade-off is that this is takes much longer
 * to run.
 */

namespace hier = hierarchy_util;
namespace ptrs = local_pointers;
namespace uv = used_vars;

class CallGraphStrategy final : public call_graph::BuildStrategy {
 public:
  explicit CallGraphStrategy(const Scope& scope)
      : m_scope(scope),
        m_non_overridden_virtuals(hier::find_non_overridden_virtuals(scope)) {}

  call_graph::CallSites get_callsites(const DexMethod* method) const override {
    call_graph::CallSites callsites;
    auto* code = const_cast<IRCode*>(method->get_code());
    if (code == nullptr) {
      return callsites;
    }
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (opcode::is_an_invoke(insn->opcode())) {
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn), method);
        if (callee == nullptr || may_be_overridden(callee)) {
          continue;
        }
        callsites.emplace_back(callee, insn);
      }
    }
    return callsites;
  }

  // XXX(jezng): We make every single method a root in order that all methods
  // are seen as reachable. Unreachable methods will not have `get_callsites`
  // run on them and will not have their outgoing edges added to the call graph,
  // which means that the dead code removal will not optimize them fully. I'm
  // not sure why these "unreachable" methods are not ultimately removed by RMU,
  // but as it stands, properly optimizing them is a size win for us.
  call_graph::RootAndDynamic get_roots() const override {
    call_graph::RootAndDynamic root_and_dynamic;
    auto& roots = root_and_dynamic.roots;

    walk::code(m_scope, [&](DexMethod* method, IRCode& code) {
      roots.emplace_back(method);
    });
    return root_and_dynamic;
  }

 private:
  bool may_be_overridden(DexMethod* method) const {
    return method->is_virtual() && m_non_overridden_virtuals.count(method) == 0;
  }

  const Scope& m_scope;
  std::unordered_set<const DexMethod*> m_non_overridden_virtuals;
};

static side_effects::InvokeToSummaryMap build_summary_map(
    const side_effects::SummaryMap& effect_summaries,
    const call_graph::Graph& call_graph,
    DexMethod* method) {
  side_effects::InvokeToSummaryMap invoke_to_summary_map;
  if (call_graph.has_node(method)) {
    const auto& callee_edges = call_graph.node(method)->callees();
    for (const auto& edge : callee_edges) {
      auto* callee = edge->callee()->method();
      if (effect_summaries.count(callee) != 0) {
        invoke_to_summary_map.emplace(edge->invoke_insn(),
                                      effect_summaries.at(callee));
      }
    }
  }
  return invoke_to_summary_map;
}

void ObjectSensitiveDcePass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  always_assert_log(
      !mgr.init_class_lowering_has_run(),
      "Implementation limitation: ObjectSensitiveDcePass could introduce new "
      "init-class instructions.");

  auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    code.build_cfg(/* editable */ false);
    // The backwards uv::FixpointIterator analysis will need it later.
    code.cfg().calculate_exit_block();
  });

  auto call_graph = call_graph::Graph(CallGraphStrategy(scope));

  ptrs::SummaryMap escape_summaries;
  if (m_external_escape_summaries_file) {
    std::ifstream file_input(*m_external_escape_summaries_file);
    summary_serialization::read(file_input, &escape_summaries);
  }
  ptrs::SummaryCMap escape_summaries_cmap(escape_summaries.begin(),
                                          escape_summaries.end());
  auto ptrs_fp_iter_map =
      ptrs::analyze_scope(scope, call_graph, &escape_summaries_cmap);

  side_effects::SummaryMap effect_summaries;
  if (m_external_side_effect_summaries_file) {
    std::ifstream file_input(*m_external_side_effect_summaries_file);
    summary_serialization::read(file_input, &effect_summaries);
  }
  side_effects::analyze_scope(init_classes_with_side_effects, scope, call_graph,
                              *ptrs_fp_iter_map, &effect_summaries);

  std::atomic<size_t> removed{0};
  std::atomic<size_t> init_class_instructions_added{0};
  init_classes::Stats init_class_stats;
  std::mutex init_class_stats_mutex;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method->rstate.no_optimizations()) {
      return;
    }

    uv::FixpointIterator used_vars_fp_iter(
        *ptrs_fp_iter_map->find(method)->second,
        build_summary_map(effect_summaries, call_graph, method),
        code.cfg());
    used_vars_fp_iter.run(uv::UsedVarsSet());

    TRACE(OSDCE, 5, "Transforming %s", SHOW(method));
    TRACE(OSDCE, 5, "Before:\n%s", SHOW(code.cfg()));
    auto dead_instructions =
        used_vars::get_dead_instructions(code, used_vars_fp_iter);
    auto local_init_class_instructions_added = 0;
    for (const auto& dead : dead_instructions) {
      // This logging is useful for quantifying what gets removed. E.g. to
      // see all the removed callsites: grep "^DEAD.*INVOKE[^ ]*" log |
      // grep " L.*$" -Po | sort | uniq -c
      TRACE(OSDCE, 3, "DEAD: %s", SHOW(dead->insn));
      auto init_class_insn =
          init_classes_with_side_effects.create_init_class_insn(
              get_init_class_type_demand(dead->insn));
      if (init_class_insn) {
        code.replace_opcode(dead, {init_class_insn});
        local_init_class_instructions_added++;
      } else {
        code.remove_opcode(dead);
      }
    }
    transform::remove_unreachable_blocks(&code);
    TRACE(OSDCE, 5, "After:\n%s", SHOW(&code));
    if (!dead_instructions.empty()) {
      removed += dead_instructions.size();
      if (local_init_class_instructions_added > 0) {
        init_class_instructions_added += local_init_class_instructions_added;
        cfg::ScopedCFG cfg(&code);
        init_classes::InitClassPruner init_class_pruner(
            init_classes_with_side_effects, method->get_class(), *cfg);
        init_class_pruner.apply();
        std::lock_guard lock_guard(init_class_stats_mutex);
        init_class_stats += init_class_pruner.get_stats();
      }
    }
  });
  mgr.set_metric("removed_instructions", removed);
  mgr.set_metric("init_class_instructions_added",
                 init_class_instructions_added);
  mgr.incr_metric("init_class_instructions_removed",
                  init_class_stats.init_class_instructions_removed);
  mgr.incr_metric("init_class_instructions_refined",
                  init_class_stats.init_class_instructions_refined);
}

static ObjectSensitiveDcePass s_pass;
