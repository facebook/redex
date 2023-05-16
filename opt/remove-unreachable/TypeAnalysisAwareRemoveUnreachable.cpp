/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisAwareRemoveUnreachable.h"

#include <boost/algorithm/string.hpp>

#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Reachability.h"
#include "Show.h"
#include "Timer.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace reachability;
namespace mog = method_override_graph;

namespace {

/*
 * We can resolve the class of an invoke-interface target. In that case, we want
 * to adjust the MethodSearch type to be Virtual.
 * TODO(zwei): remove this or the duplication in
 * TypeAnalysisCallGraphGenerationPass.
 */
MethodSearch get_method_search(const DexClass* analysis_cls,
                               IRInstruction* insn) {
  auto ms = opcode_to_search(insn);
  if (ms == MethodSearch::Interface && !is_interface(analysis_cls)) {
    ms = MethodSearch::Virtual;
  }
  return ms;
}

class TypeAnaysisAwareClosureMarker final
    : public reachability::TransitiveClosureMarker {
 public:
  explicit TypeAnaysisAwareClosureMarker(
      const IgnoreSets& ignore_sets,
      const method_override_graph::Graph& method_override_graph,
      bool record_reachability,
      ConditionallyMarked* cond_marked,
      ReachableObjects* reachable_objects,
      InstantiableTypes* instantiable_types,
      MarkWorkerState* worker_state,
      Stats* stats,
      std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> gta)
      : reachability::TransitiveClosureMarker(ignore_sets,
                                              method_override_graph,
                                              record_reachability,
                                              cond_marked,
                                              reachable_objects,
                                              instantiable_types,
                                              worker_state,
                                              stats),
        m_gta(std::move(gta)) {}

  References gather(const DexMethod* method) const override {
    References refs;
    method->gather_strings(refs.strings);
    method->gather_types(refs.types);
    method->gather_fields(refs.fields);
    // Gather from code
    gather_methods_on_code(method, refs);
    // Gather from annotations
    method->gather_methods_from_annos(refs.methods);
    return refs;
  }

  void visit_method_ref(const DexMethodRef* method) override {
    TRACE(REACH, 4, "Visiting method: %s", SHOW(method));
    auto cls = type_class(method->get_class());
    auto resolved_method = resolve_without_context(method, cls);
    if (resolved_method != nullptr) {
      TRACE(REACH, 5, "    Resolved to: %s", SHOW(resolved_method));
      this->push(method, resolved_method);
      gather_and_push(resolved_method);
    }
    push(method, method->get_class());
    push(method, method->get_proto()->get_rtype());
    for (auto const& t : *method->get_proto()->get_args()) {
      push(method, t);
    }
    if (cls && !is_abstract(cls) && method::is_init(method)) {
      instantiable(method->get_class());
    }

    auto m = method->as_def();
    if (!m || !root(m) || m->is_external()) {
      return;
    }
    // We still have to conditionally mark root overrides. RootSetMarker already
    // covers external overrides, so we skip them here.
    if (m->is_virtual() || !m->is_concrete()) {
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, m);
      if (!overriding_methods.empty()) {
        TRACE(REACH, 3, "root with overrides: %zu %s",
              overriding_methods.size(), SHOW(m));
      }
      for (auto* overriding : overriding_methods) {
        push_if_instantiable(overriding);
        TRACE(REACH, 3, "marking root override: %s", SHOW(overriding));
      }
    }
  }

 private:
  DexMethod* resolve_callee(const DexMethod* caller,
                            IRInstruction* invoke) const {
    return resolve_method(invoke->get_method(), opcode_to_search(invoke),
                          m_resolved_refs, caller);
  }

  bool is_potentially_true_virtual(const DexMethod* resolved_callee,
                                   IRInstruction* invoke) const {
    if (resolved_callee == nullptr) {
      // There are unresolvable invoke-virtuals referencing a base type.
      return opcode::is_invoke_virtual(invoke->opcode());
    }

    return mog::is_true_virtual(m_method_override_graph, resolved_callee) &&
           !opcode::is_invoke_super(invoke->opcode());
  }

  void gather_methods_on_virtual_call(const DexTypeEnvironment& env,
                                      DexMethod* resolved_callee,
                                      IRInstruction* invoke,
                                      References& refs) const {
    TRACE(TRMU, 5, "Gathering method from true virtual call %s", SHOW(invoke));
    auto* callee_ref = invoke->get_method();
    // If we failed to resolve the callee earlier and we know this is might be a
    // true virtual call, resolve the callee in a more conservative way to
    // ensure we don't miss potential callees.
    if (resolved_callee == nullptr &&
        opcode::is_invoke_virtual(invoke->opcode())) {
      resolved_callee = resolve_without_context(
          callee_ref, type_class(callee_ref->get_class()));
    }
    // Push the resolved method ref
    TRACE(TRMU, 5, "Push resolved callee %s", SHOW(resolved_callee));
    refs.methods.push_back(resolved_callee);

    auto domain = env.get(invoke->src(0));
    auto analysis_cls = domain.get_dex_cls();
    DexMethod* analysis_resolved_callee = nullptr;
    if (analysis_cls) {
      auto method_search = get_method_search(*analysis_cls, invoke);
      analysis_resolved_callee =
          resolve_method(*analysis_cls, callee_ref->get_name(),
                         callee_ref->get_proto(), method_search);
      TRACE(TRMU, 5, "Analysis type %s", SHOW(*analysis_cls));
      if (analysis_resolved_callee) {
        if (analysis_resolved_callee != resolved_callee) {
          TRACE(TRMU, 5, "Push analysis resolved callee %s",
                SHOW(analysis_resolved_callee));
          refs.methods.push_back(analysis_resolved_callee);
        }
        resolved_callee = analysis_resolved_callee;
        TRACE(TRMU, 5, "Resolved callee %s for analysis cls %s",
              SHOW(resolved_callee), SHOW(*analysis_cls));
      } else {
        // If the analysis type is too generic and we cannot resolve a concrete
        // callee based on that type, we fall back to the method reference at
        // the call site.
        TRACE(TRMU, 5, "Unresolved callee at %s for analysis cls %s",
              SHOW(invoke), SHOW(*analysis_cls));
      }
    }
    always_assert(!opcode::is_invoke_super(invoke->opcode()));
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, resolved_callee);
    for (auto overriding_method : overriding_methods) {
      TRACE(TRMU, 5, "Gather conditional method ref %s",
            SHOW(overriding_method));
      refs.cond_methods.push_back(overriding_method);
    }
  }

  void gather_methods_on_code(const DexMethod* method, References& refs) const {
    auto* code = const_cast<IRCode*>(method->get_code());
    if (code == nullptr) {
      return;
    }
    if (!code->cfg_built()) {
      code->gather_methods(refs.methods);
      return;
    }
    auto lta = m_gta->get_local_analysis(method);
    for (const auto& block : code->cfg().blocks()) {
      auto env = lta->get_entry_state_at(block);
      if (env.is_bottom()) {
        // Unreachable
        continue;
      }
      for (auto& mie : InstructionIterable(block)) {
        auto* insn = mie.insn;
        // Replay analysis for individual instruction
        lta->analyze_instruction(insn, &env);
        if (!opcode::is_an_invoke(insn->opcode())) {
          continue;
        }
        auto* resolved_callee = this->resolve_callee(method, insn);
        if (!is_potentially_true_virtual(resolved_callee, insn)) {
          // Gather declared method ref
          auto* method_ref = insn->get_method();
          always_assert(method_ref);
          refs.methods.push_back(method_ref);
          TRACE(TRMU, 5, "Gather non-true-virtual at %s resolved as %s",
                SHOW(insn), SHOW(resolved_callee));
          continue;
        }
        gather_methods_on_virtual_call(env, resolved_callee, insn, refs);
      }
    }
  }

  std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> m_gta;
  mutable MethodRefCache m_resolved_refs;
};

std::unique_ptr<ReachableObjects> compute_reachable_objects_with_type_anaysis(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    bool record_reachability,
    std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> gta,
    bool /*unused*/) {
  Timer t("Marking");
  auto scope = build_class_scope(stores);
  // Rebuild uneditable CFGs.
  walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  auto reachable_objects = std::make_unique<ReachableObjects>();
  InstantiableTypes instantiable_types;
  ConditionallyMarked cond_marked;
  auto method_override_graph = mog::build_graph(scope);

  ConcurrentSet<ReachableObject, ReachableObjectHash> root_set;
  RootSetMarker root_set_marker(*method_override_graph, record_reachability,
                                &cond_marked, reachable_objects.get(),
                                &root_set);
  root_set_marker.mark(scope);

  size_t num_threads = redex_parallel::default_num_threads();
  auto stats_arr = std::make_unique<Stats[]>(num_threads);
  workqueue_run<ReachableObject>(
      [&](MarkWorkerState* worker_state, const ReachableObject& obj) {
        TypeAnaysisAwareClosureMarker transitive_closure_marker(
            ignore_sets, *method_override_graph, record_reachability,
            &cond_marked, reachable_objects.get(), &instantiable_types,
            worker_state, &stats_arr[worker_state->worker_id()], gta);
        transitive_closure_marker.visit(obj);
        return nullptr;
      },
      root_set,
      num_threads,
      /*push_tasks_while_running=*/true);

  if (num_ignore_check_strings != nullptr) {
    for (size_t i = 0; i < num_threads; ++i) {
      *num_ignore_check_strings += stats_arr[i].num_ignore_check_strings;
    }
  }

  return reachable_objects;
}

} // namespace

std::unique_ptr<reachability::ReachableObjects>
TypeAnalysisAwareRemoveUnreachablePass::compute_reachable_objects(
    const DexStoresVector& stores,
    PassManager& pm,
    int* num_ignore_check_strings,
    bool emit_graph_this_run,
    bool remove_no_argument_constructors) {
  // Fetch analysis result
  auto analysis = pm.template get_preserved_analysis<GlobalTypeAnalysisPass>();
  always_assert(analysis);
  auto gta = analysis->get_result();
  always_assert(gta);

  return compute_reachable_objects_with_type_anaysis(
      stores, m_ignore_sets, num_ignore_check_strings, emit_graph_this_run, gta,
      remove_no_argument_constructors);
}

static TypeAnalysisAwareRemoveUnreachablePass s_pass;
