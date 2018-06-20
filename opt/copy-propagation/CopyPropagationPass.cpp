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
#include "IROpcode.h"
#include "IRTypeChecker.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Walkers.h"

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

using namespace copy_propagation_impl;
using namespace aliased_registers;

namespace {

// Represents a register that may be wide.
// There are three valid states:
//   {-, -}      =  none
//   {r, -}      =  narrow
//   {r, r + 1}  =  wide
struct RegisterPair {
  Value lower;
  Value upper;
};

class AliasFixpointIterator final
    : public MonotonicFixpointIterator<cfg::GraphInterface, AliasDomain> {
 public:
  const CopyPropagationPass::Config& m_config;
  const std::unordered_set<const IRInstruction*>& m_range_set;
  Stats& m_stats;

  AliasFixpointIterator(
      cfg::ControlFlowGraph& cfg,
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
  void run_on_block(cfg::Block* block,
                    AliasedRegisters& aliases,
                    std::unordered_set<IRInstruction*>* deletes) const {

    const auto& iterable = InstructionIterable(block);
    for (auto it = iterable.begin(); it != iterable.end(); ++it) {
      auto insn = it->insn;
      auto op = insn->opcode();

      if (m_config.replace_with_representative && deletes != nullptr) {
        replace_with_representative(insn, aliases);
      }

      const RegisterPair& src = get_src_value(insn);
      const RegisterPair& dst = get_dest_reg(it, iterable.end());

      if (!src.lower.is_none() && !dst.lower.is_none()) {
        if (aliases.are_aliases(dst.lower, src.lower) &&
            (dst.upper == src.upper || // Don't ask `aliases` about Value::none
             aliases.are_aliases(dst.upper, src.upper))) {
          // insn is a no-op. Delete it.
          if (deletes != nullptr) {
            if (opcode::is_move_result_pseudo(op)) {
              // WARNING: This assumes that the primary instruction of a
              // move-result-pseudo has no side effects.
              deletes->insert(
                  ir_list::primary_instruction_of_move_result_pseudo(
                      it.unwrap()));
            } else {
              deletes->insert(insn);
            }
          }
        } else {
          // move dst into src's alias group
          aliases.move(dst.lower, src.lower);
          if (dst.upper != src.upper) { // Don't ask `aliases` about Value::none
            aliases.move(dst.upper, src.upper);
          }
        }
      } else if (!dst.lower.is_none()) {
        // dest is being written to but not by a simple move from another
        // register or a constant load. Break its aliases because we don't
        // know what its value is.
        aliases.break_alias(dst.lower);
        if (!dst.upper.is_none()) {
          aliases.break_alias(dst.upper);
        }
      }

      // Result register can only be used by move-result(-pseudo).
      // Clear it after the move-result(-pseudo) has been processed
      if (opcode::is_move_result_pseudo(op) || is_move_result(op)) {
        aliases.break_alias(Value::create_register(RESULT_REGISTER));
        if (insn->dest_is_wide()) {
          aliases.break_alias(Value::create_register(RESULT_REGISTER + 1));
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
    IROpcode op = insn->opcode();
    if (insn->srcs_size() > 0 &&
        m_range_set.count(insn) == 0 && // range has to stay in order
        // we need to make sure the dest and src of check-cast stay identical,
        // because the dest is simply an alias to the src. See the comments in
        // IRInstruction.h for details.
        op != OPCODE_CHECK_CAST &&
        // The ART verifier checks that monitor-{enter,exit} instructions use
        // the same register:
        // http://androidxref.com/6.0.0_r5/xref/art/runtime/verifier/register_line.h#325
        !is_monitor(op)) {
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        Register r = insn->src(i);
        Register rep = get_rep(r, aliases, get_max_addressable(insn, i));
        if (rep != r) {
          // Make sure the upper half of the wide pair is also aliased.
          if (insn->src_is_wide(i)) {

            // We don't give a `max_addressable` register to get_rep() because
            // the upper half of a register is never addressed in IR
            Register upper = get_rep(r + 1, aliases, boost::none);

            if (upper != rep + 1) {
              continue;
            }
          }
          insn->set_src(i, rep);
          m_stats.replaced_sources++;
        }
      }
    }
  }

  Register get_rep(Register orig,
                   AliasedRegisters& aliases,
                   const boost::optional<Register>& max_addressable) const {
    auto val = Value::create_register(orig);
    Register rep = aliases.get_representative(val, max_addressable);
    if (rep < RESULT_REGISTER) {
      return rep;
    }
    return orig;
  }

  // return the highest allowed source register for this instruction.
  // `none` means no limit.
  Register get_max_addressable(IRInstruction* insn, size_t src_index) const {
    IROpcode op = insn->opcode();
    auto src_bit_width =
        dex_opcode::src_bit_width(opcode::to_dex_opcode(op), src_index);
    // 2 ** width - 1
    Register max_addressable_reg = (1 << src_bit_width) - 1;
    if (m_config.regalloc_has_run) {
      // We have to be careful not to create an instruction like this
      //
      //   invoke-virtual v15 Lcom;.foo:(J)V
      //
      // because lowering to Dex Instructions would change it to
      //
      //   invoke-virtual v15, v16 Lcom;.foo:(J)V
      //
      // which is a malformed instruction (v16 is too big).
      //
      // Normally, RegAlloc handles this case, but CopyProp can run after
      // RegAlloc
      bool upper_is_addressable = is_invoke(op) && insn->src_is_wide(src_index);
      return max_addressable_reg - (upper_is_addressable ? 1 : 0);
    }
    return max_addressable_reg;
  }

  // if insn has a destination register (including RESULT), return it.
  //
  // ALL destinations must be returned by this method (unlike get_src_value) if
  // we miss a destination register, we'll fail to clobber it and think we know
  // that a register holds a stale value.
  RegisterPair get_dest_reg(ir_list::InstructionIterator it,
                            ir_list::InstructionIterator end) const {
    IRInstruction* insn = it->insn;
    RegisterPair dest;

    if (is_invoke(insn->opcode()) || insn->has_move_result_pseudo()) {
      dest.lower = Value::create_register(RESULT_REGISTER);

      // It's easier to check the following move-result for the width of the
      // RESULT_REGISTER
      auto next = std::next(it);
      if (next != end &&
          (is_move_result(next->insn->opcode()) ||
           opcode::is_move_result_pseudo(next->insn->opcode())) &&
          next->insn->dest_is_wide()) {
        dest.upper = Value::create_register(RESULT_REGISTER + 1);
      }
    } else if (insn->dests_size()) {
      dest.lower = Value::create_register(insn->dest());
      if (insn->dest_is_wide()) {
        dest.upper = Value::create_register(insn->dest() + 1);
      }
    }
    return dest;
  }

  // if the source of `insn` should be tracked by CopyProp, return it
  RegisterPair get_src_value(IRInstruction* insn) const {
    RegisterPair source;
    auto op = insn->opcode();

    switch (insn->opcode()) {
    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT:
      source.lower = Value::create_register(insn->src(0));
      break;
    case OPCODE_MOVE_WIDE:
      if (m_config.wide_registers) {
        source.lower = Value::create_register(insn->src(0));
        source.upper = Value::create_register(insn->src(0) + 1);
      }
      break;
    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT:
    case IOPCODE_MOVE_RESULT_PSEUDO:
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
      source.lower = Value::create_register(RESULT_REGISTER);
      break;
    case OPCODE_MOVE_RESULT_WIDE:
    case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
      if (m_config.wide_registers) {
        source.lower = Value::create_register(RESULT_REGISTER);
        source.upper = Value::create_register(RESULT_REGISTER + 1);
      }
      break;
    case OPCODE_CONST:
      if (m_config.eliminate_const_literals) {
        source.lower = Value::create_literal(insn->get_literal());
      }
      break;
    case OPCODE_CONST_WIDE:
      if (m_config.eliminate_const_literals && m_config.wide_registers) {
        source.lower = Value::create_literal(insn->get_literal());
        source.upper = Value::create_literal_upper(insn->get_literal());
      }
      break;
    case OPCODE_CONST_STRING: {
      if (m_config.eliminate_const_strings) {
        DexString* str = insn->get_string();
        source.lower = Value{str};
      }
      break;
    }
    case OPCODE_CONST_CLASS: {
      if (m_config.eliminate_const_classes) {
        DexType* type = insn->get_type();
        source.lower = Value{type};
      }
      break;
    }
    case OPCODE_SGET:
    case OPCODE_SGET_WIDE:
    case OPCODE_SGET_OBJECT:
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_SHORT:
      if (m_config.static_finals) {
        DexField* field = resolve_field(insn->get_field(), FieldSearch::Static);
        // non-final fields could have been written to since we last made an
        // alias. Exclude them.
        if (field != nullptr && is_final(field->get_access())) {
          if (op != OPCODE_SGET_WIDE) {
            source.lower = Value::create_field(field);
          } else if (m_config.wide_registers) {
            source.lower = Value::create_field(field);
            source.upper = Value::create_field_upper(field);
          }
        }
      }
    default:
      break;
    }

    return source;
  }

  void analyze_node(cfg::Block* const& node,
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
  return walk::parallel::reduce_methods<Data, Output>(
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
      m_config.debug ? 1 : walk::parallel::default_num_threads());
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

  std::unordered_set<IRInstruction*> deletes;
  Stats stats;

  code->build_cfg();
  const auto& blocks = code->cfg().blocks();

  AliasFixpointIterator fixpoint(code->cfg(), m_config, range_set, stats);

  fixpoint.run(AliasDomain());
  for (auto block : blocks) {
    AliasDomain domain = fixpoint.get_entry_state_at(block);
    domain.update([&fixpoint, block, &deletes](AliasedRegisters& aliases) {
      fixpoint.run_on_block(block, aliases, &deletes);
    });
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
  m_config.regalloc_has_run = mgr.regalloc_has_run();

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
