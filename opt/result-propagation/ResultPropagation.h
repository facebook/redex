/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "Resolver.h"

/*
 * An index into the list of load-param instructions.
 */
using ParamIndex = uint32_t;

/*
 * A helper function that computes the mapping of load param instructions
 * to their respective indices.
 */
const std::unordered_map<const IRInstruction*, ParamIndex> get_load_param_map(
    cfg::ControlFlowGraph& cfg);

/*
 * A helper class for figuring out whether the regular return value of
 * methods and invocations is always a particular incoming parameter.
 */
class ReturnParamResolver {
 public:
  ReturnParamResolver(const method_override_graph::Graph& graph)
      : m_graph(graph),
        m_string_builder_type(DexType::make_type("Ljava/lang/StringBuilder;")),
        m_string_to_string_method(DexMethod::make_method(
            "Ljava/lang/String;.toString:()Ljava/lang/String;")) {}

  /*
   * For an invocation given by an instruction, figure out whether
   * it will always return one of its incoming sources.
   */
  const boost::optional<ParamIndex> get_return_param_index(
      IRInstruction* insn,
      const std::unordered_map<const DexMethod*, ParamIndex>&
          methods_which_return_parameter,
      MethodRefCache& resolved_refs) const;

  /*
   * For a method given by its cfg, figure out whether all regular return
   * instructions would return a particular incoming parameter.
   */
  const boost::optional<ParamIndex> get_return_param_index(
      cfg::ControlFlowGraph& cfg,
      const std::unordered_map<const DexMethod*, ParamIndex>&
          methods_which_return_parameter) const;

 private:
  bool returns_receiver(const DexMethodRef* method) const;

  const method_override_graph::Graph& m_graph;
  const DexType* m_string_builder_type;
  const DexMethodRef* m_string_to_string_method;
};

/*
 * Helper class that patches code based on analysis results.
 */
class ResultPropagation {
 public:
  struct Stats {
    size_t erased_move_results{0};
    size_t patched_move_results{0};
    size_t unverifiable_move_results{0};
  };

  ResultPropagation(const std::unordered_map<const DexMethod*, ParamIndex>&
                        methods_which_return_parameter,
                    const ReturnParamResolver& resolver)
      : m_methods_which_return_parameter(methods_which_return_parameter),
        m_resolver(resolver) {}

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  void patch(PassManager&, IRCode*);

 private:
  const std::unordered_map<const DexMethod*, ParamIndex>&
      m_methods_which_return_parameter;
  const ReturnParamResolver& m_resolver;
  mutable Stats m_stats;
  mutable MethodRefCache m_resolved_refs;
};

/*
 * This pass...
 * 1. identifies all methods which always return one of their incoming
 *    parameters
 * 2. turns all move-result-... into move instructions if the result of an
 *    invoke instruction can be predicted using the information computed in the
 *    first step.
 */
class ResultPropagationPass : public Pass {
 public:
  ResultPropagationPass() : Pass("ResultPropagationPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  /*
   * Via a fixed point computation that repeatedly inspects all methods,
   * figure out all methods which return an incoming parameter, taking into
   * account deep call chains.
   */
  static const std::unordered_map<const DexMethod*, ParamIndex>
  find_methods_which_return_parameter(PassManager& mgr,
                                      const Scope& scope,
                                      const ReturnParamResolver& resolver);
};
