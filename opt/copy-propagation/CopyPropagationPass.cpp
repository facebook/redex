/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "CopyPropagationPass.h"

#include <boost/optional.hpp>

#include "AliasedRegisters.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "FixpointIterators.h"
#include "IRInstruction.h"
#include "IRTypeChecker.h"
#include "ParallelWalkers.h"
#include "PassManager.h"

// This pass eliminates writes to registers that already hold the written value.
//
// For example,
//   move-object/from16 v0, v33
//   iget-object v2, v0, LX/04b;.a:Landroid/content/Context; // field@05d6
//   move-object/from16 v0, v33
//   iget-object v3, v0, LX/04b;.b:Ljava/lang/String; // field@05d7
//   move-object/from16 v0, v33
//   iget-object v4, v0, LX/04b;.c:LX/04K; // field@05d8
//   move-object/from16 v0, v33
//
// It keeps moving v33 to v0 even though they hold the same object!
//
// This optimization transforms the above code to this:
//   move-object/from16 v0, v33
//   iget-object v2, v0, LX/04b;.a:Landroid/content/Context; // field@05d6
//   iget-object v3, v0, LX/04b;.b:Ljava/lang/String; // field@05d7
//   iget-object v4, v0, LX/04b;.c:LX/04K; // field@05d8
//
// It does so by examinining all the writes to registers in a basic block, if vA
// is moved into vB, then vA and vB are aliases until one of them is written
// with a different value. Any move between registers that are already aliased
// is unneccesary. Eliminate them.
//
// It can also do the same thing with constant loads, if enabled by the config.
//
// This optimization can also replace source registers with a representative
// register (a whole alias group has a single representative). The reason is
// that if we use fewer registers, DCE could clean up some more moves after
// us. Another reason is that representatives are likely to be v15 or less,
// leading to more compact move instructions.
//
// Possible additions: (TODO?)
//   wide registers (I tried it. it's only a tiny help. --jhendrick)

using namespace copy_propagation_impl;

namespace {

class AliasFixpointIterator final
    : public MonotonicFixpointIterator<cfg::GraphInterface, AliasDomain> {
 public:
  const CopyPropagationPass::Config& m_config;
  const std::unordered_set<const IRInstruction*>& m_range_set;
  Stats& m_stats;

  AliasFixpointIterator(
      ControlFlowGraph& cfg,
      const CopyPropagationPass::Config& config,
      const std::unordered_set<const IRInstruction*>& range_set,
      Stats& stats)
      : MonotonicFixpointIterator<cfg::GraphInterface, AliasDomain>(
            cfg, cfg.blocks().size()),
        m_config(config),
        m_range_set(range_set),
        m_stats(stats) {}

  // An instruction can be removed if we know the source and destination are
  // aliases.
  //
  // if deletes is not null, this time is for real.
  // fill the `deletes` vector with redundant instructions
  // if deletes is null, analyze only. Make no changes to the code.
  void run_on_block(Block* block,
                    AliasedRegisters& aliases,
                    std::vector<IRInstruction*>* deletes) const {

    for (auto it = block->begin(); it != block->end(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      auto insn = it->insn;
      auto op = insn->opcode();

      if (m_config.replace_with_representative && deletes != nullptr &&
          m_config.all_representatives) {
        replace_with_representative(insn, aliases);
      }

      RegisterValue src;
      if (opcode::is_move_result_pseudo(op)) {
        src = get_src_value(primary_instruction_of_move_result_pseudo(it));
      } else if (!insn->has_move_result_pseudo()) {
        src = get_src_value(insn);
      }

      if (src != RegisterValue::none()) {
        // either a move or a constant load into `dst`
        RegisterValue dst = RegisterValue{insn->dest()};
        if (aliases.are_aliases(dst, src)) {
          if (deletes != nullptr) {
            if (opcode::is_move_result_pseudo(op)) {
              deletes->push_back(primary_instruction_of_move_result_pseudo(it));
            } else {
              deletes->push_back(insn);
            }
          }
        } else if (m_config.all_transitives) {
          // move dst into src's alias group
          aliases.move(dst, src);
        } else {
          aliases.break_alias(dst);
          aliases.make_aliased(dst, src);
        }
      } else {
        if (m_config.replace_with_representative && deletes != nullptr &&
            !m_config.all_representatives) {
          replace_with_representative(insn, aliases);
        }
        if (insn->dests_size() > 0) {
          // dest is being written to but not by a simple move from another
          // register or a constant load. Break its aliases because we don't
          // know what its value is.
          RegisterValue dst{insn->dest()};
          aliases.break_alias(dst);
          if (insn->dest_is_wide()) {
            Register wide_reg = insn->dest() + 1;
            RegisterValue wide{wide_reg};
            aliases.break_alias(wide);
          }
        }
      }
    }
  }

  // Each group of aliases has one representative register.
  // Try to replace source registers with their representative.
  //
  // We can use fewer registers and instructions if we only use one
  // register of an alias group (AKA representative)
  //
  // Example:
  //   const v0, 0
  //   const v1, 0
  //   invoke-static v0 foo
  //   invoke-static v1 bar
  //
  // Can be optimized to
  //   const v0, 0
  //   invoke-static v0 foo
  //   invoke-static v0 bar
  void replace_with_representative(IRInstruction* insn,
                                   AliasedRegisters& aliases) const {
    if (insn->srcs_size() > 0 &&
        m_range_set.count(insn) == 0 && // range has to stay in order
        // we need to make sure the dest and src of check-cast stay identical,
        // because the dest is simply an alias to the src. See the comments in
        // IRInstruction.h for details.
        insn->opcode() != OPCODE_CHECK_CAST &&
        // The ART verifier checks that monitor-{enter,exit} instructions use
        // the same register:
        // http://androidxref.com/6.0.0_r5/xref/art/runtime/verifier/register_line.h#325
        !is_monitor(insn->opcode())) {
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        RegisterValue val{insn->src(i)};
        boost::optional<Register> rep = aliases.get_representative(val);
        if (rep) {
          // filter out uses of wide registers where the second register isn't
          // also aliased
          if (insn->src_is_wide(i)) {
            RegisterValue orig_wide{(Register)(insn->src(i) + 1)};
            RegisterValue rep_wide{(Register)(*rep + 1)};
            bool wides_are_aliased = aliases.are_aliases(orig_wide, rep_wide);
            if (!wides_are_aliased) {
              continue;
            }
          }

          if (insn->src(i) != *rep) {
            insn->set_src(i, *rep);
            m_stats.replaced_sources++;
          }
        }
      }
    }
  }

  const RegisterValue get_src_value(IRInstruction* insn) const {
    switch (insn->opcode()) {
    case OPCODE_MOVE:
    case OPCODE_MOVE_FROM16:
    case OPCODE_MOVE_16:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_OBJECT_FROM16:
    case OPCODE_MOVE_OBJECT_16:
      return RegisterValue{insn->src(0)};
    case OPCODE_CONST:
    case OPCODE_CONST_4:
    case OPCODE_CONST_16:
      if (m_config.eliminate_const_literals) {
        return RegisterValue{insn->get_literal()};
      } else {
        return RegisterValue::none();
      }
    case OPCODE_CONST_STRING:
    case OPCODE_CONST_STRING_JUMBO: {
      if (m_config.eliminate_const_strings) {
        DexString* str = insn->get_string();
        return RegisterValue{str};
      } else {
        return RegisterValue::none();
      }
    }
    case OPCODE_CONST_CLASS: {
      if (m_config.eliminate_const_classes) {
        DexType* type = insn->get_type();
        return RegisterValue{type};
      } else {
        return RegisterValue::none();
      }
    }
    default:
      return RegisterValue::none();
    }
  }

  void analyze_node(Block* const& node,
                    AliasDomain* current_state) const override {
    current_state->update([&](AliasedRegisters& aliases) {
      run_on_block(node, aliases, nullptr);
    });
  }

  AliasDomain analyze_edge(
      const EdgeId&, const AliasDomain& exit_state_at_source) const override {
    return exit_state_at_source;
  }
};

} // namespace

namespace copy_propagation_impl {

Stats Stats::operator+(const Stats& other) {
  return Stats{moves_eliminated + other.moves_eliminated,
               replaced_sources + other.replaced_sources};
}

Stats CopyPropagation::run(Scope scope) {
  using Data = std::nullptr_t;
  using Output = Stats;
  return walk_methods_parallel<Data, Output>(
      scope,
      [this](Data&, DexMethod* m) {
        IRCode* code = m->get_code();
        if (code == nullptr) {
          return Stats();
        }

        const std::string& before_code =
            m_config.debug ? show(m->get_code()) : "";
        const auto& result = run(code);

        if (m_config.debug) {
          // Run the IR type checker
          IRTypeChecker checker(m);
          checker.run();
          if (!checker.good()) {
            std::string msg = checker.what();
            TRACE(RME,
                  1,
                  "%s: Inconsistency in Dex code. %s\n",
                  SHOW(m),
                  msg.c_str());
            TRACE(RME, 1, "before code:\n%s\n", before_code.c_str());
            TRACE(RME, 1, "after  code:\n%s\n", SHOW(m->get_code()));
            always_assert(false);
          }
        }
        return result;
      },
      [](Output a, Output b) { return a + b; },
      [](unsigned int /* thread_index */) { return nullptr; },
      Output(),
      m_config.debug ? 1 : walkers_default_num_threads());
}

Stats CopyPropagation::run(IRCode* code) {
  // XXX HACK! Since this pass runs after RegAlloc, we need to avoid remapping
  // registers that belong to /range instructions. The easiest way to find out
  // which instructions are in this category is by temporarily denormalizing
  // the registers.
  std::unordered_set<const IRInstruction*> range_set;
  for (auto& mie : InstructionIterable(code)) {
    auto* insn = mie.insn;
    if (opcode::has_range_form(insn->opcode())) {
      insn->denormalize_registers();
      if (needs_range_conversion(insn)) {
        range_set.emplace(insn);
      }
      insn->normalize_registers();
    }
  }

  std::vector<IRInstruction*> deletes;
  Stats stats;

  code->build_cfg();
  const auto& blocks = code->cfg().blocks();

  AliasFixpointIterator fixpoint(code->cfg(), m_config, range_set, stats);

  if (m_config.full_method_analysis) {
    fixpoint.run(AliasDomain());
    for (auto block : blocks) {
      AliasDomain domain = fixpoint.get_entry_state_at(block);
      domain.update([&fixpoint, block, &deletes](AliasedRegisters& aliases) {
        fixpoint.run_on_block(block, aliases, &deletes);
      });
    }
  } else {
    for (auto block : blocks) {
      AliasedRegisters aliases;
      fixpoint.run_on_block(block, aliases, &deletes);
    }
  }

  stats.moves_eliminated += deletes.size();
  for (auto insn : deletes) {
    code->remove_opcode(insn);
  }
  return stats;
}

} // namespace copy_propagation_impl

void CopyPropagationPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& /* unused */,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);

  if (m_config.eliminate_const_literals && !mgr.verify_none_enabled()) {
    // This option is not safe with the verifier
    m_config.eliminate_const_literals = false;
    TRACE(RME,
          1,
          "Ignoring eliminate_const_literals because verify-none is not "
          "enabled.\n");
  }

  CopyPropagation impl(m_config);
  auto stats = impl.run(scope);
  mgr.incr_metric("redundant_moves_eliminated", stats.moves_eliminated);
  mgr.incr_metric("source_regs_replaced_with_representative",
                  stats.replaced_sources);
  TRACE(RME,
        1,
        "%d redundant moves eliminated\n",
        mgr.get_metric("redundant_moves_eliminated"));
  TRACE(RME,
        1,
        "%d source registers replaced with representative\n",
        mgr.get_metric("source_regs_replaced_with_representative"));
}

static CopyPropagationPass s_pass;
