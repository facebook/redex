/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CallGraph.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "HashedAbstractPartition.h"

namespace constant_propagation {

namespace interprocedural {

/*
 * ArgumentDomain describes the constant-valued arguments (if any) for a given
 * method or callsite. The n'th argument will be represented by a binding of n
 * to a ConstantDomain instance.
 *
 * Note that while this is structurally identical to the
 * ConstantRegisterEnvironment, they should be treated as semantically
 * distinct types: Here, the environment variables denote param index, whereas
 * in a ConstantRegisterEnvironment, they denote registers.
 */
using param_index_t = uint16_t;

using ArgumentDomain =
    sparta::PatriciaTreeMapAbstractEnvironment<param_index_t, ConstantValue>;

/*
 * This map is an abstraction of the execution paths starting from the entry
 * point of a method and ending at an invoke instruction, hence the use of an
 * abstract partitioning.
 *
 * At method entry, this map contains a single item, a binding of the null
 * pointer to an ArgumentDomain representing the input arguments. At method
 * exit, this map will have bindings from all the invoke-* instructions
 * contained in the method to the ArgumentDomains representing the arguments
 * passed to the callee.
 */
using Domain =
    sparta::HashedAbstractPartition<const IRInstruction*, ArgumentDomain>;

constexpr IRInstruction* CURRENT_PARTITION_LABEL = nullptr;

/*
 * Return an environment populated with parameter values.
 */
ConstantEnvironment env_with_params(bool is_static,
                                    const IRCode* code,
                                    const ArgumentDomain& args);

using ProcedureAnalysisFactory =
    std::function<std::unique_ptr<intraprocedural::FixpointIterator>(
        const DexMethod*, const WholeProgramState&, ArgumentDomain)>;

/*
 * Performs interprocedural constant propagation of stack / register values.
 *
 * The intraprocedural propagation logic is delegated to the
 * ProcedureAnalysisFactory.
 */
class FixpointIterator : public sparta::ParallelMonotonicFixpointIterator<
                             call_graph::GraphInterface,
                             Domain> {
 public:
  FixpointIterator(const call_graph::Graph& call_graph,
                   const ProcedureAnalysisFactory& proc_analysis_factory)
      : ParallelMonotonicFixpointIterator(call_graph),
        m_proc_analysis_factory(proc_analysis_factory),
        m_call_graph(call_graph) {
    auto wps = new WholeProgramState();
    wps->set_to_top();
    m_wps.reset(wps);
  }

  void analyze_node(const call_graph::NodeId& node,
                    Domain* current_state) const override;

  Domain analyze_edge(const std::shared_ptr<call_graph::Edge>& edge,
                      const Domain& exit_state_at_source) const override;

  std::unique_ptr<intraprocedural::FixpointIterator>
  get_intraprocedural_analysis(const DexMethod*) const;

  const WholeProgramState& get_whole_program_state() const { return *m_wps; }

  void set_whole_program_state(std::unique_ptr<WholeProgramState> wps) {
    m_wps = std::move(wps);
  }

  const call_graph::Graph& get_call_graph() { return m_call_graph; }

 private:
  std::unique_ptr<const WholeProgramState> m_wps;
  ProcedureAnalysisFactory m_proc_analysis_factory;
  call_graph::Graph m_call_graph;
};

} // namespace interprocedural

/*
 * For each static field in :cls, bind its encoded value in :env.
 */
void set_encoded_values(const DexClass* cls, ConstantEnvironment* env);

void set_ifield_values(const DexClass* cls,
                       const EligibleIfields& eligible_ifields,
                       ConstantEnvironment* env);

} // namespace constant_propagation
