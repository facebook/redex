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
#include "PassManager.h"
#include "Resolver.h"
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
  this->throw_null_methods += that.throw_null_methods;
  this->abstracted_classes += that.abstracted_classes;
  this->abstracted_vmethods += that.abstracted_vmethods;
  this->removed_vmethods += that.removed_vmethods;
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
  REPORT(throw_null_methods);
  REPORT(abstracted_classes);
  REPORT(abstracted_vmethods);
  REPORT(removed_vmethods);
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
               !is_native(cls) && !root(cls)) {
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
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_SUPER:
      // Note that we don't want to call resolve_method here: The most precise
      // class information is already present in the supplied method reference,
      // which gives us the best change of finding an uninstantiable type.
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

    if ((opcode::is_an_iget(op) || opcode::is_an_iput(op)) &&
        scoped_uninstantiable_types.count(insn->get_field()->get_class())) {
      auto tmp = get_scratch();
      m.replace(it, {ir_const(tmp, 0), ir_throw(tmp)});
      stats.field_accesses_on_uninstantiable++;
      continue;
    }

    if ((opcode::is_an_iget(op) || opcode::is_an_sget(op)) &&
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
  stats.throw_null_methods++;
  return stats;
}

void RemoveUninstantiablesPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  std::unordered_set<DexType*> scoped_uninstantiable_types =
      compute_scoped_uninstantiable_types(scope);
  // We perform structural changes, i.e. whether a method has a body and
  // removal, as a post-processing step, to streamline the main operations
  struct ClassPostProcessing {
    std::unordered_map<DexMethod*, DexMethod*> remove_vmethods;
    std::unordered_set<DexMethod*> abstract_vmethods;
  };
  ConcurrentMap<DexClass*, ClassPostProcessing> class_post_processing;
  Stats stats = walk::parallel::methods<Stats>(
      scope,
      [&scoped_uninstantiable_types,
       &class_post_processing](DexMethod* method) -> Stats {
        Stats stats;

        auto code = method->get_code();
        if (method->rstate.no_optimizations() || code == nullptr) {
          return stats;
        }

        code->build_cfg();
        if (!is_static(method) &&
            scoped_uninstantiable_types.count(method->get_class())) {
          auto overridden_method =
              method->is_virtual()
                  ? resolve_method(method, MethodSearch::Super, method)
                  : nullptr;
          if (overridden_method == nullptr && method->is_virtual()) {
            class_post_processing.update(
                type_class(method->get_class()),
                [method](DexClass*, ClassPostProcessing& cpp, bool) {
                  cpp.abstract_vmethods.insert(method);
                });
            stats.abstracted_vmethods++;
          } else if (overridden_method != nullptr && can_delete(method) &&
                     (is_protected(method) || is_public(overridden_method))) {
            always_assert(overridden_method != method);
            class_post_processing.update(
                type_class(method->get_class()),
                [method,
                 overridden_method](DexClass*, ClassPostProcessing& cpp, bool) {
                  cpp.remove_vmethods.emplace(method, overridden_method);
                });
            stats.removed_vmethods++;
          } else {
            stats += replace_all_with_throw(code->cfg());
          }
        } else {
          stats += replace_uninstantiable_refs(scoped_uninstantiable_types,
                                               code->cfg());
        }
        code->clear_cfg();

        return stats;
      });

  // Post-processing:
  // 1. make methods abstract (stretty straightforward), and
  // 2. remove methods (per class in parallel for best performance, and rewrite
  // all invocation references)
  std::vector<DexClass*> classes_with_removed_vmethods;
  std::unordered_map<DexMethodRef*, DexMethodRef*> removed_vmethods;
  for (auto& p : class_post_processing) {
    auto cls = p.first;
    auto& cpp = p.second;
    if (!cpp.abstract_vmethods.empty()) {
      if (!is_abstract(cls)) {
        stats.abstracted_classes++;
        cls->set_access((cls->get_access() & ~ACC_FINAL) | ACC_ABSTRACT);
      }
      for (auto method : cpp.abstract_vmethods) {
        method->set_access((DexAccessFlags)(
            (method->get_access() & ~ACC_FINAL) | ACC_ABSTRACT));
        method->set_code(nullptr);
      }
    }
    if (!cpp.remove_vmethods.empty()) {
      classes_with_removed_vmethods.push_back(cls);
      removed_vmethods.insert(cpp.remove_vmethods.begin(),
                              cpp.remove_vmethods.end());
    }
  }

  walk::parallel::classes(classes_with_removed_vmethods,
                          [&class_post_processing](DexClass* cls) {
                            auto& cpp = class_post_processing.at_unsafe(cls);
                            for (auto& p : cpp.remove_vmethods) {
                              cls->remove_method_definition(p.first);
                            }
                          });

  // Forward chains.
  using iterator = std::unordered_map<DexMethodRef*, DexMethodRef*>::iterator;
  std::function<DexMethodRef*(iterator&)> forward;
  forward = [&forward, &removed_vmethods](iterator& it) {
    auto it2 = removed_vmethods.find(it->second);
    if (it2 != removed_vmethods.end()) {
      it->second = forward(it2);
    }
    return it->second;
  };
  for (auto it = removed_vmethods.begin(); it != removed_vmethods.end(); it++) {
    forward(it);
  }

  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    editable_cfg_adapter::iterate(&code, [&](MethodItemEntry& mie) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
        auto it = removed_vmethods.find(insn->get_method());
        if (it != removed_vmethods.end()) {
          insn->set_method(it->second);
        }
      }
      always_assert(!insn->has_method() ||
                    !removed_vmethods.count(insn->get_method()));
      return editable_cfg_adapter::LOOP_CONTINUE;
    });
  });

  stats.report(mgr);
}

static RemoveUninstantiablesPass s_pass;
