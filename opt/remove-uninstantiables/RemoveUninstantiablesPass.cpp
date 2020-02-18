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

/// \return a new \c IRInstruction representing a `check-cast` operation,
/// verifying that \p src is compatible with \p type.
IRInstruction* ir_check_cast(uint32_t src, DexType* type) {
  auto insn = new IRInstruction(OPCODE_CHECK_CAST);
  insn->set_src(0, src);
  insn->set_type(type);
  return insn;
}

/// \return a new \c IRInstruction representing a `move-result-pseudo-object`
/// operation.
IRInstruction* ir_move_result_pseudo_object(uint32_t dest) {
  auto insn = new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  insn->set_dest(dest);
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
  this->check_casts += that.check_casts;
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
  REPORT(check_casts);

#undef REPORT
}

// Computes set of uninstantiable types, also looking at the type system to
// find non-external (and non-native)...
// - interfaces that are not annotations, are not root (or unrenameable) and
//   do not contain root (or unrenameable) methods and have no non-abstract
//   classes implementing them, and
// - abstract (non-interface) classes that are not extended by any non-abstract
//   class
std::unordered_set<DexType*>
RemoveUninstantiablesPass::compute_scoped_uninstantiable_types(
    const Scope& scope) {
  // First, we compute types that might possibly be uninstantiable, and classes
  // that we consider instantiable.
  std::unordered_set<DexType*> uninstantiable_types;
  std::unordered_set<const DexClass*> instantiable_classes;
  auto is_interface_instantiable = [](const DexClass* interface) {
    if (is_annotation(interface) || interface->is_external() ||
        is_native(interface) || root(interface) || !can_rename(interface)) {
      return true;
    }
    for (auto method : interface->get_vmethods()) {
      if (root(method) || !can_rename(method)) {
        return true;
      }
    }
    return false;
  };
  walk::classes(scope, [&](const DexClass* cls) {
    if (type::is_uninstantiable_class(cls->get_type())) {
      uninstantiable_types.insert(cls->get_type());
    } else if (is_interface(cls) && !is_interface_instantiable(cls)) {
      uninstantiable_types.insert(cls->get_type());
    } else if (is_abstract(cls) && !is_interface(cls) && !cls->is_external() &&
               !is_native(cls)) {
      uninstantiable_types.insert(cls->get_type());
    } else {
      instantiable_classes.insert(cls);
    }
  });
  // Next, we prune the list of possibly uninstantiable types by looking at
  // what instantiable classes implement and extend.
  std::unordered_set<const DexClass*> visited;
  std::function<bool(const DexClass*)> visit;
  visit = [&](const DexClass* cls) {
    if (cls == nullptr || !visited.insert(cls).second) {
      return false;
    }
    uninstantiable_types.erase(cls->get_type());
    for (auto interface : cls->get_interfaces()->get_type_list()) {
      visit(type_class(interface));
    }
    return true;
  };
  for (auto cls : instantiable_classes) {
    while (visit(cls)) {
      cls = type_class(cls->get_super_class());
    }
  }
  uninstantiable_types.insert(type::java_lang_Void());
  return uninstantiable_types;
}

RemoveUninstantiablesPass::Stats
RemoveUninstantiablesPass::replace_uninstantiable_refs(
    const std::unordered_set<DexType*>& scoped_uninstantiable_types,
    cfg::ControlFlowGraph& cfg) {
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
      if (scoped_uninstantiable_types.count(insn->get_type())) {
        auto dest = cfg.move_result_of(it)->insn->dest();
        m.replace(it, {ir_const(dest, 0)});
        stats.instance_ofs++;
      }
      continue;

    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL:
      if (scoped_uninstantiable_types.count(insn->get_method()->get_class())) {
        auto tmp = get_scratch();
        m.replace(it, {ir_const(tmp, 0), ir_throw(tmp)});
        stats.invokes++;
      }
      continue;

    case OPCODE_CHECK_CAST:
      if (scoped_uninstantiable_types.count(insn->get_type())) {
        auto src = insn->src(0);
        auto dest = cfg.move_result_of(it)->insn->dest();
        m.replace(it,
                  {ir_check_cast(src, type::java_lang_Void()),
                   ir_move_result_pseudo_object(dest), ir_const(src, 0),
                   ir_const(dest, 0)});
        stats.check_casts++;
      }
      continue;

    default:
      break;
    }

    if ((is_iget(op) || is_iput(op)) &&
        scoped_uninstantiable_types.count(insn->get_field()->get_class())) {
      auto tmp = get_scratch();
      m.replace(it, {ir_const(tmp, 0), ir_throw(tmp)});
      stats.field_accesses_on_uninstantiable++;
      continue;
    }

    if ((is_iget(op) || is_sget(op)) &&
        scoped_uninstantiable_types.count(insn->get_field()->get_type())) {
      auto dest = cfg.move_result_of(it)->insn->dest();
      m.replace(it, {ir_const(dest, 0)});
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
  std::unordered_set<DexType*> scoped_uninstantiable_types =
      compute_scoped_uninstantiable_types(scope);
  Stats stats = walk::parallel::methods<Stats>(
      scope, [&scoped_uninstantiable_types](DexMethod* method) -> Stats {
        Stats stats;

        auto code = method->get_code();
        if (method->rstate.no_optimizations() || code == nullptr) {
          return stats;
        }

        code->build_cfg();
        if (!is_static(method) &&
            scoped_uninstantiable_types.count(method->get_class())) {
          stats += replace_all_with_throw(code->cfg());
        } else {
          stats += replace_uninstantiable_refs(scoped_uninstantiable_types,
                                               code->cfg());
        }
        code->clear_cfg();

        return stats;
      });

  stats.report(mgr);
}

static RemoveUninstantiablesPass s_pass;
