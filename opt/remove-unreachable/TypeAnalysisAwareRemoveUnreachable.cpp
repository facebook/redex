/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisAwareRemoveUnreachable.h"

#include <boost/algorithm/string.hpp>

#include "ControlFlow.h"
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

struct MethodReferences {
  std::vector<DexMethodRef*> methods;
  std::vector<const DexMethod*>
      exact_invoke_virtual_targets_if_class_instantiable;
  const DexMethod* base_invoke_virtual_target_if_class_instantiable{nullptr};
  const DexMethod* invoke_super_target{nullptr};
  void add_to(References* refs) const {
    refs->methods.insert(refs->methods.end(), methods.begin(), methods.end());
    refs->exact_invoke_virtual_targets_if_class_instantiable.insert(
        exact_invoke_virtual_targets_if_class_instantiable.begin(),
        exact_invoke_virtual_targets_if_class_instantiable.end());
    if (base_invoke_virtual_target_if_class_instantiable) {
      refs->base_invoke_virtual_targets_if_class_instantiable.insert(
          base_invoke_virtual_target_if_class_instantiable);
    }
    if (invoke_super_target) {
      refs->invoke_super_targets.insert(invoke_super_target);
    }
  }
};

using InsnsMethods = std::unordered_map<const IRInstruction*, MethodReferences>;

struct TypeAnalysisAwareClosureMarkerSharedState final
    : public reachability::TransitiveClosureMarkerSharedState {
  type_analyzer::global::GlobalTypeAnalyzer* gta;
  mutable std::atomic<int> num_exact_resolved_callees{0};

  void gather_mie(const std::shared_ptr<InsnsMethods>& insns_methods_cache,
                  const DexMethod* method,
                  const MethodItemEntry& mie,
                  reachability::References* refs) const {
    bool default_gather_methods =
        mie.type != MFLOW_OPCODE || !opcode::is_an_invoke(mie.insn->opcode());
    MethodReferencesGatherer::default_gather_mie(this, method, mie, refs,
                                                 default_gather_methods);
    if (!default_gather_methods) {
      if (insns_methods_cache->empty()) {
        *insns_methods_cache = gather_methods_on_insns(method);
      }
      insns_methods_cache->at(mie.insn).add_to(refs);
    }
  }

 private:
  bool is_potentially_true_virtual(const DexMethod* resolved_callee,
                                   IRInstruction* invoke) const {
    if (resolved_callee == nullptr) {
      // There are unresolvable invoke-virtuals referencing a base type.
      return opcode::is_invoke_virtual(invoke->opcode());
    }

    return mog::is_true_virtual(*method_override_graph, resolved_callee) &&
           !opcode::is_invoke_super(invoke->opcode());
  }

  void gather_methods_on_virtual_call(const DexTypeEnvironment& env,
                                      DexMethod* resolved_callee,
                                      IRInstruction* invoke,
                                      MethodReferences& refs) const {
    TRACE(TRMU, 5, "Gathering method from true virtual call %s", SHOW(invoke));
    always_assert(!opcode::is_invoke_super(invoke->opcode()));
    auto* callee_ref = invoke->get_method();
    // If we failed to resolve the callee earlier and we know this is might be a
    // true virtual call, resolve the callee in a more conservative way to
    // ensure we don't miss potential callees.
    if (resolved_callee == nullptr &&
        opcode::is_invoke_virtual(invoke->opcode())) {
      resolved_callee = const_cast<DexMethod*>(resolve_without_context(
          callee_ref, type_class(callee_ref->get_class())));
    }
    // Push the resolved method ref
    TRACE(TRMU, 5, "Push resolved callee %s", SHOW(resolved_callee));
    refs.methods.push_back(resolved_callee);

    auto domain = env.get(invoke->src(0));

    // Can we leverage exact types?
    const auto& set_domain = domain.get_set_domain();
    if (!set_domain.is_top()) {
      const auto& types = set_domain.get_types();
      if (!types.contains(type::java_lang_Throwable())) {
        std::vector<std::pair<DexClass*, DexMethod*>> analysis_resolved_callees;
        for (auto* type : types) {
          auto analysis_cls = type_class(type);
          if (!analysis_cls) {
            break;
          }
          always_assert(!is_interface(analysis_cls));
          auto method_search = get_method_search(analysis_cls, invoke);
          auto analysis_resolved_callee =
              resolve_method(analysis_cls, callee_ref->get_name(),
                             callee_ref->get_proto(), method_search);
          if (!analysis_resolved_callee) {
            break;
          }
          analysis_resolved_callees.emplace_back(analysis_cls,
                                                 analysis_resolved_callee);
        }
        if (analysis_resolved_callees.size() == types.size()) {
          for (auto [analysis_cls, analysis_resolved_callee] :
               analysis_resolved_callees) {
            TRACE(TRMU, 5, "Exact resolved callee %s for analysis cls %s",
                  SHOW(analysis_resolved_callee), SHOW(analysis_cls));
            always_assert(analysis_resolved_callee->is_virtual());
            if (!analysis_resolved_callee->is_external()) {
              always_assert(!is_abstract(analysis_resolved_callee));
              refs.exact_invoke_virtual_targets_if_class_instantiable.push_back(
                  analysis_resolved_callee);
            }
          }
          num_exact_resolved_callees++;
          return;
        }
      }
    }

    // Can we leverage best known approximation?
    auto analysis_cls = domain.get_dex_cls();
    DexMethod* analysis_resolved_callee = nullptr;
    if (analysis_cls) {
      auto method_search = get_method_search(*analysis_cls, invoke);
      analysis_resolved_callee =
          resolve_method(*analysis_cls, callee_ref->get_name(),
                         callee_ref->get_proto(), method_search);
      TRACE(TRMU, 5, "Analysis type %s", SHOW(*analysis_cls));
      if (analysis_resolved_callee) {
        TRACE(TRMU, 5, "Push analysis resolved callee %s",
              SHOW(analysis_resolved_callee));
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

    if (!resolved_callee) {
      // Typically clone() on an array, or other obscure external references
      TRACE(TRMU, 2, "Unresolved callee at %s without analysis cls",
            SHOW(invoke));
      return;
    }

    always_assert(resolved_callee);
    always_assert(refs.base_invoke_virtual_target_if_class_instantiable ==
                  nullptr);
    refs.base_invoke_virtual_target_if_class_instantiable = resolved_callee;
  }

  InsnsMethods gather_methods_on_insns(const DexMethod* method) const {
    InsnsMethods insns_refs;
    auto* code = const_cast<IRCode*>(method->get_code());
    always_assert(code);
    always_assert(code->editable_cfg_built());
    auto lta = gta->get_local_analysis(method);
    MethodRefCache resolved_refs;
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
        auto* resolved_callee = resolve_method(
            insn->get_method(), opcode_to_search(insn), resolved_refs, method);
        auto& refs = insns_refs[insn];
        if (!is_potentially_true_virtual(resolved_callee, insn)) {
          // Gather declared method ref
          auto* method_ref = insn->get_method();
          always_assert(method_ref);
          refs.methods.push_back(method_ref);
          if (opcode::is_invoke_super(insn->opcode()) && resolved_callee &&
              !resolved_callee->is_external()) {
            always_assert(resolved_callee->is_virtual());
            always_assert(refs.invoke_super_target == nullptr);
            if (is_abstract(resolved_callee)) {
              TRACE(REACH, 1,
                    "invoke super target of {%s} is abstract method %s in %s",
                    SHOW(insn), SHOW(resolved_callee), SHOW(method));
            } else {
              refs.invoke_super_target = resolved_callee;
            }
          } else if (resolved_callee && resolved_callee->is_virtual() &&
                     !resolved_callee->is_external()) {
            always_assert(!is_abstract(resolved_callee));
            refs.exact_invoke_virtual_targets_if_class_instantiable.push_back(
                resolved_callee);
          }
          TRACE(TRMU, 5, "Gather non-true-virtual at %s resolved as %s",
                SHOW(insn), SHOW(resolved_callee));
          continue;
        }
        gather_methods_on_virtual_call(env, resolved_callee, insn, refs);
      }
    }
    return insns_refs;
  }
};

class TypeAnalysisAwareClosureMarkerWorker final
    : public reachability::TransitiveClosureMarkerWorker {
 public:
  TypeAnalysisAwareClosureMarkerWorker(
      const TypeAnalysisAwareClosureMarkerSharedState* shared_state,
      reachability::TransitiveClosureMarkerWorkerState* worker_state)
      : reachability::TransitiveClosureMarkerWorker(shared_state, worker_state),
        m_shared_state(shared_state) {}

  void gather_and_push(const DexMethod* method) override {
    GatherMieFunction gather_mie =
        std::bind(&TypeAnalysisAwareClosureMarkerSharedState::gather_mie,
                  m_shared_state,
                  std::make_shared<InsnsMethods>(),
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3);
    reachability::TransitiveClosureMarkerWorker::gather_and_push(
        create_method_references_gatherer(method,
                                          /* consider_code */ true,
                                          std::move(gather_mie)),
        reachability::MethodReferencesGatherer::Advance::initial());
  }

 private:
  const TypeAnalysisAwareClosureMarkerSharedState* const m_shared_state;
};

std::unique_ptr<ReachableObjects> compute_reachable_objects_with_type_anaysis(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    ReachableAspects* reachable_aspects,
    bool record_reachability,
    bool relaxed_keep_class_members,
    bool cfg_gathering_check_instantiable,
    bool cfg_gathering_check_instance_callable,
    type_analyzer::global::GlobalTypeAnalyzer* gta,
    bool /*unused*/,
    int* num_exact_resolved_callees) {
  Timer t("Marking");
  auto scope = build_class_scope(stores);
  walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
    code.cfg().calculate_exit_block();
  });
  auto reachable_objects = std::make_unique<ReachableObjects>();
  ConditionallyMarked cond_marked;
  auto method_override_graph = mog::build_graph(scope);

  ConcurrentSet<ReachableObject, ReachableObjectHash> root_set;
  bool remove_no_argument_constructors = false;
  RootSetMarker root_set_marker(*method_override_graph, record_reachability,
                                relaxed_keep_class_members,
                                remove_no_argument_constructors, &cond_marked,
                                reachable_objects.get(), &root_set);
  root_set_marker.mark(scope);

  size_t num_threads = redex_parallel::default_num_threads();
  Stats stats;
  TypeAnalysisAwareClosureMarkerSharedState shared_state{
      {&ignore_sets, method_override_graph.get(), record_reachability,
       relaxed_keep_class_members, cfg_gathering_check_instantiable,
       cfg_gathering_check_instance_callable, &cond_marked,
       reachable_objects.get(), reachable_aspects, &stats},
      gta};

  workqueue_run<ReachableObject>(
      [&](TransitiveClosureMarkerWorkerState* worker_state,
          const ReachableObject& obj) {
        TypeAnalysisAwareClosureMarkerWorker worker(&shared_state,
                                                    worker_state);
        worker.visit(obj);
        return nullptr;
      },
      root_set, num_threads,
      /* push_tasks_while_running*/ true);
  compute_zombie_methods(*method_override_graph, *reachable_objects,
                         *reachable_aspects);

  if (num_ignore_check_strings != nullptr) {
    *num_ignore_check_strings = (int)stats.num_ignore_check_strings;
  }
  if (num_exact_resolved_callees != nullptr) {
    *num_exact_resolved_callees = (int)shared_state.num_exact_resolved_callees;
  }

  reachable_aspects->finish(cond_marked, *reachable_objects);

  return reachable_objects;
}

} // namespace

std::unique_ptr<reachability::ReachableObjects>
TypeAnalysisAwareRemoveUnreachablePass::compute_reachable_objects(
    const DexStoresVector& stores,
    PassManager& pm,
    int* num_ignore_check_strings,
    reachability::ReachableAspects* reachable_aspects,
    bool emit_graph_this_run,
    bool relaxed_keep_class_members,
    bool cfg_gathering_check_instantiable,
    bool cfg_gathering_check_instance_callable,
    bool remove_no_argument_constructors) {
  // Fetch analysis result
  auto analysis = pm.template get_preserved_analysis<GlobalTypeAnalysisPass>();
  always_assert(analysis);
  auto gta = analysis->get_result();
  always_assert(gta);

  int num_exact_resolved_callees;
  auto res = compute_reachable_objects_with_type_anaysis(
      stores, m_ignore_sets, num_ignore_check_strings, reachable_aspects,
      emit_graph_this_run, relaxed_keep_class_members,
      cfg_gathering_check_instantiable, cfg_gathering_check_instance_callable,
      gta.get(), remove_no_argument_constructors, &num_exact_resolved_callees);
  pm.incr_metric("num_exact_resolved_callees", num_exact_resolved_callees);
  TRACE(TRMU, 1, "num_exact_resolved_callees %d", num_exact_resolved_callees);
  return res;
}

static TypeAnalysisAwareRemoveUnreachablePass s_pass;
