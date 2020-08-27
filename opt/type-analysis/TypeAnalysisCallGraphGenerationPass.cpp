/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisCallGraphGenerationPass.h"

#include "DexUtil.h"
#include "GlobalTypeAnalysisPass.h"
#include "MethodOverrideGraph.h"
#include "Walkers.h"

using namespace call_graph;
using namespace type_analyzer;

namespace mog = method_override_graph;

namespace {

class TypeAnalysisBasedStrategy : public MultipleCalleeBaseStrategy {
 public:
  explicit TypeAnalysisBasedStrategy(
      const Scope& scope,
      std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> gta)
      : MultipleCalleeBaseStrategy(scope), m_gta(std::move(gta)) {
    walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
      code.build_cfg(/* editable */ false);
      code.cfg().calculate_exit_block();
    });
  }

  CallSites get_callsites(const DexMethod* method) const override {
    CallSites callsites;
    auto* code = const_cast<IRCode*>(method->get_code());
    if (code == nullptr) {
      return callsites;
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
        if (resolved_callee == nullptr) {
          continue;
        }
        if (!is_definitely_virtual(resolved_callee)) {
          // Not true virtual call
          if (resolved_callee->is_concrete()) {
            callsites.emplace_back(resolved_callee, code->iterator_to(mie));
          }
        } else {
          get_callsites_for_true_virtual_call(code, resolved_callee, env, mie,
                                              callsites);
        }
      }
    }
    return callsites;
  }

 protected:
  void get_callsites_for_true_virtual_call(IRCode* code,
                                           DexMethod* resolved_callee,
                                           const DexTypeEnvironment& env,
                                           MethodItemEntry& invoke,
                                           CallSites& callsites) const {
    auto* insn = invoke.insn;
    auto* callee_ref = insn->get_method();
    auto domain = env.get(insn->src(0));
    auto analysis_cls = domain.get_dex_cls();
    if (analysis_cls) {
      resolved_callee =
          resolve_method(*analysis_cls, callee_ref->get_name(),
                         callee_ref->get_proto(), opcode_to_search(insn));
    }
    // Add callees to callsites
    if (resolved_callee->is_concrete()) {
      callsites.emplace_back(resolved_callee, code->iterator_to(invoke));
    }
    if (insn->opcode() != OPCODE_INVOKE_SUPER) {
      const auto& overriding_methods = mog::get_overriding_methods(
          *m_method_override_graph, resolved_callee);
      for (auto overriding_method : overriding_methods) {
        callsites.emplace_back(overriding_method, code->iterator_to(invoke));
      }
    }
  }

  std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> m_gta;
};

} // namespace

void TypeAnalysisCallGraphGenerationPass::run_pass(DexStoresVector& stores,
                                                   ConfigFiles& config,
                                                   PassManager& mgr) {
  auto analysis = mgr.get_preserved_analysis<GlobalTypeAnalysisPass>();
  always_assert(analysis);
  auto gta = analysis->get_result();
  always_assert(gta);

  Scope scope = build_class_scope(stores);
  m_result = std::make_shared<call_graph::Graph>(
      TypeAnalysisBasedStrategy(scope, gta));
}

static TypeAnalysisCallGraphGenerationPass s_pass;
