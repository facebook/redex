/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObjectSensitiveDcePass.h"

#include <fstream>
#include <functional>

#include "CFGMutation.h"
#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "HierarchyUtil.h"
#include "InitClassPruner.h"
#include "InitClassesWithSideEffects.h"
#include "LocalPointersAnalysis.h"
#include "PassManager.h"
#include "Purity.h"
#include "ScopedCFG.h"
#include "SummarySerialization.h"
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

namespace {

class CallGraphStrategy final : public call_graph::MultipleCalleeStrategy {
 public:
  explicit CallGraphStrategy(
      const method_override_graph::Graph& graph,
      const Scope& scope,
      const std::unordered_set<DexMethodRef*>& pure_methods,
      const ptrs::SummaryMap& escape_summaries,
      const side_effects::SummaryMap& effect_summaries,
      uint32_t big_override_threshold)
      : call_graph::MultipleCalleeStrategy(
            graph, scope, big_override_threshold),
        m_pure_methods(pure_methods),
        m_escape_summaries(escape_summaries),
        m_effect_summaries(effect_summaries) {
    // XXX(jezng): We make every single method a root in order that all methods
    // are seen as reachable. Unreachable methods will not have `get_callsites`
    // run on them and will not have their outgoing edges added to the call
    // graph, which means that the dead code removal will not optimize them
    // fully. I'm not sure why these "unreachable" methods are not ultimately
    // removed by RMU, but as it stands, properly optimizing them is a size win
    // for us.
    m_root_and_dynamic = MultipleCalleeStrategy::get_roots();
    walk::code(m_scope, [&](DexMethod* method, IRCode&) {
      m_root_and_dynamic.roots.insert(method);
    });
  }

  bool is_pure(IRInstruction* insn) const {
    // This is what LocalDce does.
    auto ref = insn->get_method();
    const auto meth = resolve_method(ref, opcode_to_search(insn));
    if (meth == nullptr) {
      return false;
    }
    if (::assumenosideeffects(meth)) {
      return true;
    }
    return m_pure_methods.count(ref);
  }

  call_graph::CallSites get_callsites(const DexMethod* method) const override {
    call_graph::CallSites callsites;
    auto* code = const_cast<IRCode*>(method->get_code());
    if (code == nullptr) {
      return callsites;
    }
    always_assert(code->editable_cfg_built());
    for (auto& mie : InstructionIterable(code->cfg())) {
      auto insn = mie.insn;
      if (!opcode::is_an_invoke(insn->opcode())) {
        continue;
      }
      auto callee = resolve_invoke_method(insn, method);
      if (callee == nullptr) {
        if (ptrs::is_array_clone(insn->get_method())) {
          // We'll synthesize appropriate summaries for array clone methods on
          // the fly.
          callsites.emplace_back(/* callee */ nullptr, insn);
        }
        continue;
      }
      if (is_pure(insn)) {
        // By including this in the call-graph with an empty callee, it will by
        // default get trivial summaries, representing no interactions with
        // objects, and no side effects.
        callsites.emplace_back(/* callee */ nullptr, insn);
        continue;
      }
      if (callee->is_external()) {
        if ((opcode::is_invoke_super(insn->opcode()) ||
             !ptrs::may_be_overridden(callee)) &&
            has_summaries(callee)) {
          callsites.emplace_back(callee, insn);
        }
        continue;
      }

      if (is_definitely_virtual(callee) &&
          insn->opcode() != OPCODE_INVOKE_SUPER) {
        if (m_root_and_dynamic.dynamic_methods.count(callee)) {
          continue;
        }

        // For true virtual callees, add the callee itself and all of its
        // overrides if they are not in big virtuals.
        if (m_big_virtuals.count_unsafe(callee)) {
          continue;
        }
        const auto& overriding_methods =
            get_ordered_overriding_methods_with_code_or_native(callee);
        if (is_native(callee) ||
            std::any_of(overriding_methods.begin(), overriding_methods.end(),
                        [](auto* method) { return is_native(method); })) {
          continue;
        }
        if (callee->get_code()) {
          callsites.emplace_back(callee, insn);
        }
        for (auto overriding_method : overriding_methods) {
          callsites.emplace_back(overriding_method, insn);
        }
      } else if (callee->is_concrete() && !is_native(callee)) {
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
    return m_root_and_dynamic;
  }

 private:
  bool has_summaries(DexMethod* method) const {
    if (m_escape_summaries.count(method) && m_effect_summaries.count(method)) {
      return true;
    }
    return method == method::java_lang_Object_ctor();
  }

  const std::unordered_set<DexMethodRef*>& m_pure_methods;
  const ptrs::SummaryMap& m_escape_summaries;
  const side_effects::SummaryMap& m_effect_summaries;
  call_graph::RootAndDynamic m_root_and_dynamic;
};

} // namespace

void ObjectSensitiveDcePass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  always_assert_log(
      !mgr.init_class_lowering_has_run(),
      "Implementation limitation: ObjectSensitiveDcePass could introduce new "
      "init-class instructions.");

  auto scope = build_class_scope(stores);
  auto method_override_graph = method_override_graph::build_graph(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns(), method_override_graph.get());

  auto pure_methods = get_pure_methods();
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());
  auto immutable_getters = get_immutable_getters(scope);
  pure_methods.insert(immutable_getters.begin(), immutable_getters.end());

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    always_assert(code.editable_cfg_built());
    // The backwards uv::FixpointIterator analysis will need it later.
    code.cfg().calculate_exit_block();
  });

  ptrs::SummaryMap escape_summaries;
  if (m_external_escape_summaries_file) {
    std::ifstream file_input(*m_external_escape_summaries_file);
    summary_serialization::read(file_input, &escape_summaries);
  }
  mgr.incr_metric("external_escape_summaries", escape_summaries.size());

  side_effects::SummaryMap effect_summaries;
  if (m_external_side_effect_summaries_file) {
    std::ifstream file_input(*m_external_side_effect_summaries_file);
    summary_serialization::read(file_input, &effect_summaries);
  }
  mgr.incr_metric("external_side_effect_summaries", effect_summaries.size());

  auto call_graph = call_graph::Graph(CallGraphStrategy(
      *method_override_graph, scope, pure_methods, escape_summaries,
      effect_summaries, m_big_override_threshold));

  auto ptrs_fp_iter_map =
      ptrs::analyze_scope(scope, call_graph, &escape_summaries);

  side_effects::analyze_scope(init_classes_with_side_effects, scope, call_graph,
                              ptrs_fp_iter_map, &effect_summaries);

  std::atomic<size_t> removed{0};
  std::atomic<size_t> init_class_instructions_added{0};
  std::mutex init_class_stats_mutex;
  init_classes::Stats init_class_stats;
  std::mutex invokes_with_summaries_mutex;
  std::unordered_map<uint16_t, size_t> invokes_with_summaries{0};

  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method->get_code() == nullptr || method->rstate.no_optimizations()) {
      return;
    }
    always_assert(code.editable_cfg_built());
    auto& cfg = code.cfg();
    auto summary_map = build_summary_map(effect_summaries, call_graph, method);
    std::unordered_map<uint32_t, size_t> local_invokes_with_summaries;
    for (auto&& [insn, summary] : summary_map) {
      if (!summary.effects) {
        local_invokes_with_summaries[insn->opcode()]++;
      }
    }
    uv::FixpointIterator used_vars_fp_iter(*ptrs_fp_iter_map.at_unsafe(method),
                                           std::move(summary_map), cfg, method);
    used_vars_fp_iter.run(uv::UsedVarsSet());

    cfg::CFGMutation mutator(cfg);

    TRACE(OSDCE, 5, "Transforming %s", SHOW(method));
    TRACE(OSDCE, 5, "Before:\n%s", SHOW(cfg));
    auto dead_instructions =
        used_vars::get_dead_instructions(cfg, used_vars_fp_iter);
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
        mutator.replace(dead, {init_class_insn});
        local_init_class_instructions_added++;
      } else {
        mutator.remove(dead);
      }
    }

    mutator.flush();

    cfg.remove_unreachable_blocks();
    TRACE(OSDCE, 5, "After:\n%s", SHOW(cfg));
    if (!dead_instructions.empty()) {
      removed += dead_instructions.size();
      if (local_init_class_instructions_added > 0) {
        init_class_instructions_added += local_init_class_instructions_added;
        init_classes::InitClassPruner init_class_pruner(
            init_classes_with_side_effects, method->get_class(), cfg);
        init_class_pruner.apply();
        std::lock_guard lock_guard(init_class_stats_mutex);
        init_class_stats += init_class_pruner.get_stats();
      }
    }

    std::lock_guard<std::mutex> lock(invokes_with_summaries_mutex);
    for (auto&& [opcode, count] : local_invokes_with_summaries) {
      invokes_with_summaries[opcode] += count;
    }
  });
  mgr.set_metric("removed_instructions", removed);
  mgr.set_metric("init_class_instructions_added",
                 init_class_instructions_added);
  mgr.incr_metric("init_class_instructions_removed",
                  init_class_stats.init_class_instructions_removed);
  mgr.incr_metric("init_class_instructions_refined",
                  init_class_stats.init_class_instructions_refined);

  size_t methods_with_summaries{0};
  size_t modified_params{0};
  for (auto&& [_, summary] : effect_summaries) {
    if (!summary.effects) {
      methods_with_summaries++;
    }
    modified_params += summary.modified_params.size();
  }
  mgr.set_metric("methods_with_summaries", methods_with_summaries);
  mgr.set_metric("modified_params", modified_params);
  mgr.set_metric("invoke_direct_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_DIRECT]);
  mgr.set_metric("invoke_static_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_STATIC]);
  mgr.set_metric("invoke_interface_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_INTERFACE]);
  mgr.set_metric("invoke_virtual_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_VIRTUAL]);
  mgr.set_metric("invoke_super_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_SUPER]);
  TRACE(OSDCE, 1, "%zu methods with summaries, removed %zu instructions",
        methods_with_summaries, (size_t)removed);
}

static ObjectSensitiveDcePass s_pass;
