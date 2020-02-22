/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalTypeAnalyzer.h"

#include "MethodOverrideGraph.h"
#include "Resolver.h"
#include "Walkers.h"

namespace mog = method_override_graph;

using namespace type_analyzer;

namespace {

/*
 * For fields accessed by the method, populate the FieldTypeEnvironment with the
 * values from WholeProgramState.
 */
void initialize_field_env(const WholeProgramState& wps,
                          const IRCode* code,
                          DexTypeEnvironment& env,
                          std::unordered_set<DexField*>& written_fields) {
  bool populated = false;
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (!insn->has_field()) {
      continue;
    }
    auto field = resolve_field(insn->get_field());
    if (field == nullptr) {
      continue;
    }
    auto type = wps.get_field_type(field);
    if (!type.is_top()) {
      env.set(field, type);
      written_fields.insert(field);
      populated = true;
    }
  }

  if (traceEnabled(TYPE, 5) && populated) {
    std::ostringstream out;
    out << env.get_field_environment();
    TRACE(TYPE, 5, "initialized field env %s", out.str().c_str());
  }
}

} // namespace

namespace type_analyzer {

namespace global {

DexTypeEnvironment env_with_params(const IRCode* code,
                                   const ArgumentTypeEnvironment& args) {

  size_t idx = 0;
  DexTypeEnvironment env;
  for (auto& mie : InstructionIterable(code->get_param_instructions())) {
    env.set(mie.insn->dest(), args.get(idx++));
  }
  return env;
}

void GlobalTypeAnalyzer::analyze_node(
    const DexMethod* const& method,
    ArgumentTypePartition* current_partition) const {
  if (method == nullptr) {
    return;
  }
  auto code = method->get_code();
  if (code == nullptr) {
    return;
  }
  auto& cfg = code->cfg();
  auto intra_ta = get_local_analysis(method);
  const auto outgoing_edges =
      call_graph::GraphInterface::successors(m_call_graph, method);
  std::unordered_set<IRInstruction*> outgoing_insns;
  for (const auto& edge : outgoing_edges) {
    outgoing_insns.emplace(edge->invoke_iterator()->insn);
  }
  for (auto* block : cfg.blocks()) {
    auto state = intra_ta->get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->has_method() && outgoing_insns.count(insn)) {
        ArgumentTypeEnvironment out_args;
        for (size_t i = 0; i < insn->srcs_size(); ++i) {
          out_args.set(i, state.get(insn->src(i)));
        }
        current_partition->set(insn, out_args);
      }
      intra_ta->analyze_instruction(insn, &state);
    }
  }
}

ArgumentTypePartition GlobalTypeAnalyzer::analyze_edge(
    const std::shared_ptr<call_graph::Edge>& edge,
    const ArgumentTypePartition& exit_state_at_source) const {
  ArgumentTypePartition entry_state_at_dest;
  auto it = edge->invoke_iterator();
  if (it == IRList::iterator()) {
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            ArgumentTypeEnvironment::top());
  } else {
    auto insn = it->insn;
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            exit_state_at_source.get(insn));
  }
  return entry_state_at_dest;
}

std::unique_ptr<local::LocalTypeAnalyzer>
GlobalTypeAnalyzer::get_local_analysis(const DexMethod* method) const {
  auto args = this->get_entry_state_at(const_cast<DexMethod*>(method));
  return analyze_method(method,
                        this->get_whole_program_state(),
                        args.get(CURRENT_PARTITION_LABEL));
}

using CombinedAnalyzer =
    InstructionAnalyzerCombiner<WholeProgramAwareAnalyzer,
                                local::RegisterTypeAnalyzer,
                                local::FieldTypeAnalyzer>;

std::unique_ptr<local::LocalTypeAnalyzer> GlobalTypeAnalyzer::analyze_method(
    const DexMethod* method,
    const WholeProgramState& wps,
    ArgumentTypeEnvironment args) const {
  TRACE(TYPE, 5, "[global] analyzing %s", SHOW(method));
  always_assert(method->get_code() != nullptr);
  auto& code = *method->get_code();
  // Currently, our callgraph does not include calls to non-devirtualizable
  // virtual methods. So those methods may appear unreachable despite being
  // reachable.
  if (args.is_bottom()) {
    args.set_to_top();
  } else if (!args.is_top()) {
    TRACE(TYPE, 3, "Have args for %s: %s", SHOW(method), SHOW(args));
  }

  auto env = env_with_params(&code, args);
  auto written_fields = std::make_unique<std::unordered_set<DexField*>>();
  auto* wf_ptr = written_fields.get();
  if (!method::is_clinit(method)) {
    initialize_field_env(wps, &code, env, *written_fields);
  }
  TRACE(TYPE, 5, "%s", SHOW(code.cfg()));
  auto local_ta = std::make_unique<local::LocalTypeAnalyzer>(
      code.cfg(),
      CombinedAnalyzer(&wps, nullptr, wf_ptr),
      std::move(written_fields));
  local_ta->run(env);

  return local_ta;
}

std::unique_ptr<GlobalTypeAnalyzer> GlobalTypeAnalysis::analyze(
    const Scope& scope) {
  call_graph::Graph cg = call_graph::single_callee_graph(scope);
  // Rebuild all CFGs here -- this should be more efficient than doing them
  // within FixpointIterator::analyze_node(), since that can get called
  // multiple times for a given method
  walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });
  // Run the bootstrap. All field value and method return values are
  // represented by Top.
  TRACE(TYPE, 2, "[global] Bootstrap run");
  auto gta = std::make_unique<GlobalTypeAnalyzer>(cg);
  gta->run({{CURRENT_PARTITION_LABEL, ArgumentTypeEnvironment()}});
  auto non_true_virtuals = mog::get_non_true_virtuals(scope);

  for (size_t i = 0; i < m_max_global_analysis_iteration; ++i) {
    // Build an approximation of all the field values and method return values.
    TRACE(TYPE, 2, "[global] Collecting WholeProgramState");
    auto wps =
        std::make_unique<WholeProgramState>(scope, *gta, non_true_virtuals);
    // If this approximation is not better than the previous one, we are done.
    if (gta->get_whole_program_state().leq(*wps)) {
      break;
    }
    // Use the refined WholeProgramState to propagate more constants via
    // the stack and registers.
    TRACE(TYPE, 2, "[global] Start a new global analysis run");
    gta->set_whole_program_state(std::move(wps));
    gta->run({{CURRENT_PARTITION_LABEL, ArgumentTypeEnvironment()}});
  }

  return gta;
}

} // namespace global

} // namespace type_analyzer
