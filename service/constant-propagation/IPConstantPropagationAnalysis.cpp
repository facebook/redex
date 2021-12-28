/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IPConstantPropagationAnalysis.h"

namespace constant_propagation {

namespace interprocedural {

/*
 * Return an environment populated with parameter values.
 */
ConstantEnvironment env_with_params(bool is_static,
                                    const IRCode* code,
                                    const ArgumentDomain& args) {
  boost::sub_range<IRList> param_instruction =
      code->editable_cfg_built() ? code->cfg().get_param_instructions()
                                 : code->get_param_instructions();
  size_t idx{0};
  ConstantEnvironment env;
  for (auto& mie : InstructionIterable(param_instruction)) {
    auto value = args.get(idx);
    if (idx == 0 && !is_static) {
      // Use a customized meet operator for special handling the NEZ and other
      // non-null objects.
      value = meet(value, SignedConstantDomain(sign_domain::Interval::NEZ));
    }
    env.set(mie.insn->dest(), value);
    idx++;
  }
  return env;
}

void FixpointIterator::analyze_node(call_graph::NodeId const& node,
                                    Domain* current_state) const {
  const DexMethod* method = node->method();
  // The entry node has no associated method.
  if (method == nullptr) {
    return;
  }
  auto code = method->get_code();
  if (code == nullptr) {
    return;
  }
  if (!code->cfg_built()) {
    // This can happen when there are dangling references to methods that can
    // never run.
    return;
  }
  auto& cfg = code->cfg();
  auto intra_cp = get_intraprocedural_analysis(method);
  const auto outgoing_edges =
      call_graph::GraphInterface::successors(m_call_graph, node);
  std::unordered_set<IRInstruction*> outgoing_insns;
  for (const auto& edge : outgoing_edges) {
    if (edge->callee() == m_call_graph.exit()) {
      continue; // ghost edge to the ghost exit node
    }
    outgoing_insns.emplace(edge->invoke_insn());
  }
  for (auto* block : cfg.blocks()) {
    auto state = intra_cp->get_entry_state_at(block);
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->has_method()) {
        if (outgoing_insns.count(insn)) {
          ArgumentDomain out_args;
          for (size_t i = 0; i < insn->srcs_size(); ++i) {
            out_args.set(i, state.get(insn->src(i)));
          }
          current_state->set(insn, out_args);
        }
      }
      intra_cp->analyze_instruction(insn, &state, insn == last_insn->insn);
    }
  }
}

Domain FixpointIterator::analyze_edge(
    const std::shared_ptr<call_graph::Edge>& edge,
    const Domain& exit_state_at_source) const {
  Domain entry_state_at_dest;
  auto insn = edge->invoke_insn();
  if (insn == nullptr) {
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL, ArgumentDomain::top());
  } else {
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            exit_state_at_source.get(insn));
  }
  return entry_state_at_dest;
}

std::unique_ptr<intraprocedural::FixpointIterator>
FixpointIterator::get_intraprocedural_analysis(const DexMethod* method) const {
  auto args = Domain::bottom();

  if (m_call_graph.has_node(method)) {
    args = this->get_entry_state_at(m_call_graph.node(method));
  }

  return m_proc_analysis_factory(method,
                                 this->get_whole_program_state(),
                                 args.get(CURRENT_PARTITION_LABEL));
}

} // namespace interprocedural

void set_encoded_values(const DexClass* cls, ConstantEnvironment* env) {
  for (auto* sfield : cls->get_sfields()) {
    if (sfield->is_external()) {
      continue;
    }
    auto value = sfield->get_static_value();
    if (value == nullptr || value->evtype() == DEVT_NULL) {
      env->set(sfield, SignedConstantDomain(0));
    } else if (type::is_primitive(sfield->get_type())) {
      env->set(sfield, SignedConstantDomain(value->value()));
    } else if (sfield->get_type() == type::java_lang_String() &&
               value->evtype() == DEVT_STRING) {
      env->set(
          sfield,
          StringDomain(static_cast<DexEncodedValueString*>(value)->string()));
    } else if (sfield->get_type() == type::java_lang_Class() &&
               value->evtype() == DEVT_TYPE) {
      env->set(sfield,
               ConstantClassObjectDomain(
                   static_cast<DexEncodedValueType*>(value)->type()));
    } else {
      env->set(sfield, ConstantValue::top());
    }
  }
}

/**
 * Bind all eligible fields to SignedConstantDomain(0) in :env, since all
 * fields are initialized to zero by default at runtime. This function is
 * much simpler than set_ifield_values since there are no DexEncodedValues
 * to handle.
 */
void set_ifield_values(const DexClass* cls,
                       const EligibleIfields& eligible_ifields,
                       ConstantEnvironment* env) {
  always_assert(!cls->is_external());
  if (cls->get_ctors().size() > 1) {
    return;
  }
  for (auto* ifield : cls->get_ifields()) {
    if (!eligible_ifields.count(ifield)) {
      // If the field is not a eligible ifield, move on.
      continue;
    }
    env->set(ifield, SignedConstantDomain(0));
  }
}

} // namespace constant_propagation
