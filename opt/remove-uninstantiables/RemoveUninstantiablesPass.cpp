/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUninstantiablesPass.h"

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Trace.h"
#include "Walkers.h"

#include <boost/optional.hpp>

namespace {

/// \return a new \c IRInstruction representing a `const` operation writing
/// literal \p lit into register \p dest.
IRInstruction* ir_const(uint32_t dest, int64_t lit) {
  auto insn = new IRInstruction(OPCODE_CONST);
  insn->set_dest(dest);
  insn->set_literal(lit);
  return insn;
}

/// \return a new \c IRInstruction representing a `throw` operation, throwing
/// the contents of register \p src.
IRInstruction* ir_throw(uint32_t src) {
  auto insn = new IRInstruction(OPCODE_THROW);
  insn->set_src(0, src);
  return insn;
}

} // namespace

RemoveUninstantiablesPass::Stats& RemoveUninstantiablesPass::Stats::operator+=(
    const Stats& that) {
  this->instance_ofs += that.instance_ofs;
  this->invokes += that.invokes;
  this->field_accesses_on_uninstantiable +=
      that.field_accesses_on_uninstantiable;
  this->instance_methods_of_uninstantiable +=
      that.instance_methods_of_uninstantiable;
  this->get_uninstantiables += that.get_uninstantiables;
  return *this;
}

RemoveUninstantiablesPass::Stats RemoveUninstantiablesPass::Stats::operator+(
    const Stats& that) const {
  auto copy = *this;
  copy += that;
  return copy;
}

void RemoveUninstantiablesPass::Stats::report(PassManager& mgr) const {
#define REPORT(STAT)                                                       \
  do {                                                                     \
    mgr.incr_metric(#STAT, STAT);                                          \
    TRACE(RMUNINST, 2, "  " #STAT ": %d/%d", STAT, mgr.get_metric(#STAT)); \
  } while (0)

  TRACE(RMUNINST, 2, "RemoveUninstantiablesPass Stats:");

  REPORT(instance_ofs);
  REPORT(invokes);
  REPORT(field_accesses_on_uninstantiable);
  REPORT(instance_methods_of_uninstantiable);
  REPORT(get_uninstantiables);

#undef REPORT
}

RemoveUninstantiablesPass::Stats
RemoveUninstantiablesPass::replace_uninstantiable_refs(
    cfg::ControlFlowGraph& cfg) {
  using Insert = cfg::CFGMutation::Insert;
  cfg::CFGMutation m(cfg);

  // Lazily generate a scratch register.
  auto get_scratch = [&cfg, reg = boost::optional<uint32_t>()]() mutable {
    if (!reg) {
      reg = cfg.allocate_temp();
    }
    return *reg;
  };

  Stats stats;
  auto ii = InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    auto op = insn->opcode();
    switch (op) {
    case OPCODE_INSTANCE_OF:
      if (type::is_uninstantiable_class(insn->get_type())) {
        auto dest = cfg.move_result_of(it)->insn->dest();
        m.add_change(Insert::Replacing, it, {ir_const(dest, 0)});
        stats.instance_ofs++;
      }
      continue;

    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL:
      if (type::is_uninstantiable_class(insn->get_method()->get_class())) {
        auto tmp = get_scratch();
        m.add_change(Insert::Replacing, it, {ir_const(tmp, 0), ir_throw(tmp)});
        stats.invokes++;
      }
      continue;

    default:
      break;
    }

    if ((is_iget(op) || is_iput(op)) &&
        type::is_uninstantiable_class(insn->get_field()->get_class())) {
      auto tmp = get_scratch();
      m.add_change(Insert::Replacing, it, {ir_const(tmp, 0), ir_throw(tmp)});
      stats.field_accesses_on_uninstantiable++;
      continue;
    }

    if ((is_iget(op) || is_sget(op)) &&
        type::is_uninstantiable_class(insn->get_field()->get_type())) {
      auto dest = cfg.move_result_of(it)->insn->dest();
      m.add_change(Insert::Replacing, it, {ir_const(dest, 0)});
      stats.get_uninstantiables++;
      continue;
    }
  }

  m.flush();
  return stats;
}

RemoveUninstantiablesPass::Stats
RemoveUninstantiablesPass::replace_all_with_throw(cfg::ControlFlowGraph& cfg) {
  auto* entry = cfg.entry_block();
  always_assert_log(entry, "Expect an entry block");

  auto it = entry->to_cfg_instruction_iterator(
      entry->get_first_non_param_loading_insn());
  always_assert_log(!it.is_end(), "Expecting a non-param loading instruction");

  auto tmp = cfg.allocate_temp();
  cfg.insert_before(it, {ir_const(tmp, 0), ir_throw(tmp)});

  Stats stats;
  stats.instance_methods_of_uninstantiable++;
  return stats;
}

void RemoveUninstantiablesPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  Stats stats =
      walk::parallel::methods<Stats>(scope, [](DexMethod* method) -> Stats {
        Stats stats;

        auto code = method->get_code();
        if (method->rstate.no_optimizations() || code == nullptr) {
          return stats;
        }

        code->build_cfg();
        if (!is_static(method) &&
            type::is_uninstantiable_class(method->get_class())) {
          stats += replace_all_with_throw(code->cfg());
        } else {
          stats += replace_uninstantiable_refs(code->cfg());
        }
        code->clear_cfg();

        return stats;
      });

  stats.report(mgr);
}

static RemoveUninstantiablesPass s_pass;
