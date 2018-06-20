/**
 * Copyright (c) 2016-present, Facebook, Inc. * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
    PatriciaTreeMapAbstractEnvironment<param_index_t, ConstantValue>;

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
using Domain = HashedAbstractPartition<const IRInstruction*, ArgumentDomain>;

constexpr IRInstruction* CURRENT_PARTITION_LABEL = nullptr;

/*
 * Return an environment populated with parameter values.
 */
ConstantEnvironment env_with_params(const IRCode* code,
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
class FixpointIterator
    : public MonotonicFixpointIterator<call_graph::GraphInterface, Domain> {
 public:
  FixpointIterator(const call_graph::Graph& call_graph,
                   const ProcedureAnalysisFactory& proc_analysis_factory)
      : MonotonicFixpointIterator(call_graph),
        m_wps(new WholeProgramState()),
        m_proc_analysis_factory(proc_analysis_factory) {}

  void analyze_node(DexMethod* const& method,
                    Domain* current_state) const override;

  Domain analyze_edge(const std::shared_ptr<call_graph::Edge>& edge,
                      const Domain& exit_state_at_source) const override;

  std::unique_ptr<intraprocedural::FixpointIterator>
  get_intraprocedural_analysis(const DexMethod*) const;

  const WholeProgramState& get_whole_program_state() const { return *m_wps; }

  void set_whole_program_state(std::unique_ptr<WholeProgramState> wps) {
    m_wps = std::move(wps);
  }

 private:
  std::unique_ptr<const WholeProgramState> m_wps;
  ProcedureAnalysisFactory m_proc_analysis_factory;
};

} // namespace interprocedural

/*
 * For each static field in :cls, bind its encoded value in :env.
 */
void set_encoded_values(const DexClass* cls, ConstantEnvironment* env);

} // namespace constant_propagation
