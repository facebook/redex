/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IPConstantPropagationAnalysis.h"

namespace constant_propagation {

namespace interprocedural {

/*
 * Return an environment populated with parameter values.
 */
static ConstantEnvironment env_with_params(const IRCode* code,
                                           const ArgumentDomain& args) {
  size_t idx{0};
  ConstantEnvironment env;
  for (auto& mie : InstructionIterable(code->get_param_instructions())) {
    env.set(mie.insn->dest(), args.get(idx++));
  }
  return env;
}

void FixpointIterator::analyze_node(DexMethod* const& method,
                                    Domain* current_state) const {
  // The entry node has no associated method.
  if (method == nullptr) {
    return;
  }
  auto code = method->get_code();
  if (code == nullptr) {
    return;
  }
  auto& cfg = code->cfg();
  intraprocedural::FixpointIterator intra_cp(cfg, m_config, m_wps.get());
  intra_cp.run(
      env_with_params(code, current_state->get(CURRENT_PARTITION_LABEL)));

  for (auto* block : cfg.blocks()) {
    auto state = intra_cp.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      auto op = insn->opcode();
      if (op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_STATIC) {
        ArgumentDomain out_args;
        for (size_t i = 0; i < insn->srcs_size(); ++i) {
          out_args.set(i, state.get(insn->src(i)));
        }
        current_state->set(insn, out_args);
      }
      intra_cp.analyze_instruction(insn, &state);
    }
  }
}

Domain FixpointIterator::analyze_edge(
    const std::shared_ptr<call_graph::Edge>& edge,
    const Domain& exit_state_at_source) const {
  Domain entry_state_at_dest;
  auto it = edge->invoke_iterator();
  if (it == IRList::iterator()) {
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            ConstantEnvironment::top());
  } else {
    auto insn = it->insn;
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            exit_state_at_source.get(insn));
  }
  return entry_state_at_dest;
}

std::unique_ptr<intraprocedural::FixpointIterator>
FixpointIterator::get_intraprocedural_analysis(const DexMethod* method) const {
  always_assert(method->get_code() != nullptr);
  auto& code = *method->get_code();
  auto args = this->get_entry_state_at(const_cast<DexMethod*>(method));
  // Currently, our callgraph does not include calls to non-devirtualizable
  // virtual methods. So those methods may appear unreachable despite being
  // reachable.
  if (args.is_bottom()) {
    args.set_to_top();
  } else if (!args.is_top()) {
    TRACE(
        ICONSTP, 3, "Have args for %s: %s\n", SHOW(method), args.str().c_str());
  }

  auto intra_cp = std::make_unique<intraprocedural::FixpointIterator>(
      code.cfg(), m_config, &this->get_whole_program_state());
  auto env = env_with_params(&code, args.get(CURRENT_PARTITION_LABEL));
  intra_cp->run(env);

  return intra_cp;
}

} // namespace interprocedural

} // namespace constant_propagation
