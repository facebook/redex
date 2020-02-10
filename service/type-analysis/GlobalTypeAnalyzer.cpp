/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalTypeAnalyzer.h"

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
    InstructionAnalyzerCombiner<local::InstructionTypeAnalyzer,
                                WholeProgramAwareAnalyzer>;

std::unique_ptr<local::LocalTypeAnalyzer> GlobalTypeAnalyzer::analyze_method(
    const DexMethod* method,
    const WholeProgramState& wps,
    ArgumentTypeEnvironment args) const {
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
  TRACE(TYPE, 5, "[global] analyzing %s", SHOW(method));
  TRACE(TYPE, 5, "%s", SHOW(code.cfg()));
  auto local_ta = std::make_unique<local::LocalTypeAnalyzer>(
      code.cfg(), CombinedAnalyzer(nullptr, &wps));
  local_ta->run(env);

  return local_ta;
}

} // namespace global

} // namespace type_analyzer
