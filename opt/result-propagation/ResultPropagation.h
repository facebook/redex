/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "Resolver.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

/*
 * A helper class for figuring out whether the regular return value of
 * methods and invocations is always a particular incoming parameter.
 */
class ReturnParamResolver {
 public:
  explicit ReturnParamResolver(const method_override_graph::Graph& graph)
      : m_graph(graph),
        m_byte_buffer_type(DexType::make_type("Ljava/nio/ByteBuffer;")),
        m_char_buffer_type(DexType::make_type("Ljava/nio/CharBuffer;")),
        m_double_buffer_type(DexType::make_type("Ljava/nio/DoubleBuffer;")),
        m_float_buffer_type(DexType::make_type("Ljava/nio/FloatBuffer;")),
        m_int_buffer_type(DexType::make_type("Ljava/nio/IntBuffer;")),
        m_long_buffer_type(DexType::make_type("Ljava/nio/LongBuffer;")),
        m_short_buffer_type(DexType::make_type("Ljava/nio/ShortBuffer;")),
        m_print_stream_type(DexType::make_type("Ljava/lang/PrintStream;")),
        m_print_writer_type(DexType::make_type("Ljava/lang/PrintWriter;")),
        m_string_buffer_type(DexType::make_type("Ljava/lang/StringBuffer;")),
        m_string_builder_type(DexType::make_type("Ljava/lang/StringBuilder;")),
        m_string_writer_type(DexType::make_type("Ljava/lang/StringWriter;")),
        m_writer_type(DexType::make_type("Ljava/lang/Writer;")),
        m_string_to_string_method(DexMethod::make_method(
            "Ljava/lang/String;.toString:()Ljava/lang/String;")) {}

  /*
   * For an invocation given by an instruction, figure out whether
   * it will always return one of its incoming sources.
   */
  boost::optional<ParamIndex> get_return_param_index(
      const IRInstruction* insn,
      const UnorderedMap<const DexMethod*, ParamIndex>&
          methods_which_return_parameter,
      MethodRefCache& resolved_refs) const;

  /*
   * For a method given by its cfg, figure out whether all regular return
   * instructions would return a particular incoming parameter.
   */
  boost::optional<ParamIndex> get_return_param_index(
      const cfg::ControlFlowGraph& cfg,
      const UnorderedMap<const DexMethod*, ParamIndex>&
          methods_which_return_parameter) const;

 private:
  bool returns_receiver(const DexMethodRef* method) const;
  bool returns_compatible_with_receiver(const DexMethodRef* method) const;

  const method_override_graph::Graph& m_graph;
  const DexType* m_byte_buffer_type;
  const DexType* m_char_buffer_type;
  const DexType* m_double_buffer_type;
  const DexType* m_float_buffer_type;
  const DexType* m_int_buffer_type;
  const DexType* m_long_buffer_type;
  const DexType* m_short_buffer_type;
  const DexType* m_print_stream_type;
  const DexType* m_print_writer_type;
  const DexType* m_string_buffer_type;
  const DexType* m_string_builder_type;
  const DexType* m_string_writer_type;
  const DexType* m_writer_type;
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

    Stats& operator+=(const Stats& that) {
      erased_move_results += that.erased_move_results;
      patched_move_results += that.patched_move_results;
      unverifiable_move_results += that.unverifiable_move_results;
      return *this;
    }
  };

  ResultPropagation(const UnorderedMap<const DexMethod*, ParamIndex>&
                        methods_which_return_parameter,
                    const ReturnParamResolver& resolver,
                    const UnorderedSet<DexMethod*>& callee_blocklist)
      : m_methods_which_return_parameter(methods_which_return_parameter),
        m_resolver(resolver),
        m_callee_blocklist(callee_blocklist) {}

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  void patch(PassManager&, cfg::ControlFlowGraph&);

 private:
  const UnorderedMap<const DexMethod*, ParamIndex>&
      m_methods_which_return_parameter;
  const ReturnParamResolver& m_resolver;
  mutable Stats m_stats;
  mutable MethodRefCache m_resolved_refs;
  const UnorderedSet<DexMethod*>& m_callee_blocklist;
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

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
    };
  }

  void bind_config() override {
    bind("callee_blocklist",
         {},
         m_callee_blocklist,
         "Skip propagating results from selected callees.");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  UnorderedSet<DexMethod*> m_callee_blocklist;
  /*
   * Via a fixed point computation that repeatedly inspects all methods,
   * figure out all methods which return an incoming parameter, taking into
   * account deep call chains.
   */
  static UnorderedMap<const DexMethod*, ParamIndex>
  find_methods_which_return_parameter(PassManager& mgr,
                                      const Scope& scope,
                                      const ReturnParamResolver& resolver);
};
