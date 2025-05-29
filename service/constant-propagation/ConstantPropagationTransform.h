/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CFGMutation.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationState.h"
#include "ConstantPropagationWholeProgramState.h"
#include "DeterministicContainers.h"
#include "IRCode.h"
#include "Liveness.h"
#include "NullPointerExceptionUtil.h"

class ScopedMetrics;

namespace constant_propagation {

/**
 * Optimize the given code by:
 *   - removing dead branches
 *   - converting instructions to `const` when the values are known
 *   - removing field writes if they all write the same constant value
 */
class Transform final {
 public:
  struct Config {
    bool replace_moves_with_consts{true};
    bool replace_move_result_with_consts{false};
    bool remove_dead_switch{true};
    bool add_param_const{true};
    bool to_int_lit8{true};
    bool to_int_lit16{false}; // Does not seem beneficial by default.
    const DexType* class_under_init{nullptr};
    // These methods are known pure, we can replace their results with constant
    // value.
    const ConcurrentSet<DexMethod*>* getter_methods_for_immutable_fields{
        nullptr};
    const UnorderedSet<DexMethodRef*>* pure_methods{nullptr};
    Config() {}
  };

  struct Stats {
    size_t branches_removed{0};
    size_t branches_forwarded{0};
    size_t materialized_consts{0};
    size_t added_param_const{0};
    size_t throws{0};
    size_t null_checks{0};
    size_t null_checks_method_calls{0};
    size_t unreachable_instructions_removed{0};
    size_t redundant_puts_removed{0};

    Stats& operator+=(const Stats& that) {
      branches_removed += that.branches_removed;
      branches_forwarded += that.branches_forwarded;
      materialized_consts += that.materialized_consts;
      added_param_const += that.added_param_const;
      throws += that.throws;
      null_checks += that.null_checks;
      null_checks_method_calls += that.null_checks_method_calls;
      unreachable_instructions_removed += that.unreachable_instructions_removed;
      redundant_puts_removed += that.redundant_puts_removed;
      return *this;
    }

    void log_metrics(ScopedMetrics& sm, bool with_scope = true) const;
  };

  explicit Transform(Config config, const State& state)
      : m_config(config), m_state(state) {}

  // Apply all available transformations on editable cfg
  // May run cfg.calculate_exit_block as a side-effect.
  void apply(const intraprocedural::FixpointIterator& fp_iter,
             const WholeProgramState& wps,
             cfg::ControlFlowGraph& cfg,
             const XStoreRefs* xstores,
             bool is_static,
             DexType* declaring_type,
             DexProto*);

  // Apply transformations on editable cfg; don't call directly, prefer calling
  // `apply` instead.
  void legacy_apply_constants_and_prune_unreachable(
      const intraprocedural::FixpointIterator&,
      const WholeProgramState&,
      cfg::ControlFlowGraph&,
      const XStoreRefs*,
      const DexType*);

  // Apply targets-forwarding transformations on editable cfg; don't call
  // directly, prefer calling `apply` instead.
  // Runs cfg.calculate_exit_block as a side-effect.
  void legacy_apply_forward_targets(const intraprocedural::FixpointIterator&,
                                    cfg::ControlFlowGraph&,
                                    bool is_static,
                                    DexType* declaring_type,
                                    DexProto*,
                                    const XStoreRefs*);

  const Stats& get_stats() const { return m_stats; }

 private:
  /*
   * The methods in this class queue up their transformations. After they are
   * all done, the apply_changes() method does the actual modifications.
   */
  void apply_changes(cfg::ControlFlowGraph&);

  void simplify_instruction(const ConstantEnvironment&,
                            const WholeProgramState& wps,
                            const cfg::InstructionIterator& cfg_it,
                            const XStoreRefs*,
                            const DexType*);

  bool replace_with_const(const ConstantEnvironment&,
                          const cfg::InstructionIterator& cfg_it,
                          const XStoreRefs*,
                          const DexType*);
  void generate_const_param(const ConstantEnvironment&,
                            const cfg::InstructionIterator& cfg_it,
                            const XStoreRefs*,
                            const DexType*);

  bool eliminate_redundant_put(const ConstantEnvironment&,
                               const WholeProgramState& wps,
                               const cfg::InstructionIterator& cfg_it);
  bool eliminate_redundant_null_check(const ConstantEnvironment&,
                                      const WholeProgramState& wps,
                                      const cfg::InstructionIterator& cfg_it);
  bool replace_with_throw(const ConstantEnvironment&,
                          const cfg::InstructionIterator& cfg_it,
                          npe::NullPointerExceptionCreator* npe_creator);

  void remove_dead_switch(const intraprocedural::FixpointIterator& intra_cp,
                          const ConstantEnvironment&,
                          cfg::ControlFlowGraph&,
                          cfg::Block*);

  void eliminate_dead_branch(const intraprocedural::FixpointIterator&,
                             const ConstantEnvironment&,
                             cfg::ControlFlowGraph&,
                             cfg::Block*);

  void forward_targets(
      const intraprocedural::FixpointIterator&,
      const ConstantEnvironment&,
      cfg::ControlFlowGraph&,
      cfg::Block*,
      std::unique_ptr<LivenessFixpointIterator>& liveness_fixpoint_iter);

  // Check whether the code can return a value of a unavailable/external type,
  // or a type defined in a store different from the one where the method is
  // defined in.
  bool has_problematic_return(cfg::ControlFlowGraph&,
                              bool is_static,
                              DexType* declaring_type,
                              DexProto* proto,
                              const XStoreRefs*);

  bool assumenosideeffects(DexMethodRef* ref, DexMethod* meth) const;

  const Config m_config;
  std::unique_ptr<cfg::CFGMutation> m_mutation;
  std::vector<IRInstruction*> m_added_param_values;
  UnorderedSet<IRInstruction*> m_redundant_move_results;
  std::vector<cfg::Edge*> m_edge_deletes;
  std::vector<std::tuple<cfg::Block*, cfg::Block*, cfg::EdgeType>> m_edge_adds;
  Stats m_stats;

  const State& m_state;
};

/*
 * Generates an appropriate const-* instruction for a given ConstantValue.
 */
class value_to_instruction_visitor final
    : public boost::static_visitor<std::vector<IRInstruction*>> {
 public:
  explicit value_to_instruction_visitor(const IRInstruction* original,
                                        const XStoreRefs* xstores,
                                        const DexType* declaring_type)
      : m_original(original),
        m_xstores(xstores),
        m_declaring_type(declaring_type) {}

  std::vector<IRInstruction*> operator()(
      const SignedConstantDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return {};
    }
    IRInstruction* insn = new IRInstruction(
        m_original->dest_is_wide() ? OPCODE_CONST_WIDE : OPCODE_CONST);
    insn->set_literal(*cst);
    insn->set_dest(m_original->dest());
    return {insn};
  }

  std::vector<IRInstruction*> operator()(const StringDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return {};
    }
    IRInstruction* insn = new IRInstruction(OPCODE_CONST_STRING);
    insn->set_string(*cst);
    return {insn, (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                      ->set_dest(m_original->dest())};
  }

  std::vector<IRInstruction*> operator()(
      const ConstantClassObjectDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return {};
    }
    auto type = const_cast<DexType*>(*cst);
    if (!m_xstores || m_xstores->illegal_ref(m_declaring_type, type)) {
      return {};
    }
    IRInstruction* insn = new IRInstruction(OPCODE_CONST_CLASS);
    insn->set_type(type);
    return {insn, (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                      ->set_dest(m_original->dest())};
  }

  template <typename Domain>
  std::vector<IRInstruction*> operator()(const Domain& dom) const {
    return {};
  }

 private:
  const IRInstruction* m_original;
  const XStoreRefs* m_xstores;
  const DexType* m_declaring_type;
};

} // namespace constant_propagation
