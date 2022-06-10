/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinInstanceRewriter.h"
#include "CFGMutation.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Walkers.h"

namespace {
DexField* has_instance_field(DexClass* cls, const DexString* instance) {
  for (auto* filed : cls->get_sfields()) {
    if (filed->get_name() == instance && filed->get_type() == cls->get_type()) {
      return filed;
    }
  }
  return nullptr;
}

} // namespace

KotlinInstanceRewriter::Stats KotlinInstanceRewriter::collect_instance_usage(
    const Scope& scope,
    ConcurrentMap<DexFieldRef*,
                  std::set<std::pair<IRInstruction*, DexMethod*>>>&
        concurrent_instance_map,

    std::function<bool(DexClass*)> do_not_consider_type) {
  // Collect all the types which are of Kotlin classes which sets INSTANCE
  // variable.
  // Get all the uses of the INSTANCE variables whose <init> does not have
  // sideeffets
  KotlinInstanceRewriter::Stats stats{};
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (!can_rename(cls) || !can_delete(cls)) {
      return;
    }
    auto instance = has_instance_field(cls, m_instance);
    if (!instance) {
      return;
    }
    if (do_not_consider_type(cls)) {
      return;
    }
    if (concurrent_instance_map.count(instance)) {
      return;
    }
    std::set<std::pair<IRInstruction*, DexMethod*>> insns;
    concurrent_instance_map.emplace(instance, insns);
  });
  stats.kotlin_new_instance = concurrent_instance_map.size();
  return stats;
}

KotlinInstanceRewriter::Stats KotlinInstanceRewriter::remove_escaping_instance(
    const Scope& scope,
    ConcurrentMap<DexFieldRef*,
                  std::set<std::pair<IRInstruction*, DexMethod*>>>&
        concurrent_instance_map) {
  ConcurrentSet<DexFieldRef*> remove_list;
  // Get all the single uses of the INSTANCE variables
  KotlinInstanceRewriter::Stats total_stats =
      walk::parallel::methods<KotlinInstanceRewriter::Stats>(
          scope, [&](DexMethod* method) {
            KotlinInstanceRewriter::Stats stats{};
            auto code = method->get_code();
            if (!code) {
              return stats;
            }

            cfg::ScopedCFG cfg(method->get_code());
            auto iterable = cfg::InstructionIterable(*cfg);
            for (auto it = iterable.begin(); it != iterable.end(); it++) {
              auto insn = it->insn;

              if (!opcode::is_an_sget(insn->opcode()) &&
                  !opcode::is_an_sput(insn->opcode())) {
                continue;
              }

              auto field = insn->get_field();
              if (!concurrent_instance_map.count(field)) {
                continue;
              }
              if (remove_list.count(field)) {
                continue;
              }
              // If there is more SPUT otherthan the initial one.
              if (opcode::is_an_sput(insn->opcode())) {
                if (method::is_clinit(method) &&
                    method->get_class() == field->get_type()) {
                  continue;
                }
                // Erase if the field is written elsewhere.
                remove_list.insert(field);
                continue;
              }

              concurrent_instance_map.update(
                  field,
                  [&](DexFieldRef*,
                      std::set<std::pair<IRInstruction*, DexMethod*>>& s,
                      bool /* exists */) {
                    s.insert(std::make_pair(insn, method));
                  });
            }
            return stats;
          });
  for (auto* field : remove_list) {
    concurrent_instance_map.erase(field);
  }
  return total_stats;
}

KotlinInstanceRewriter::Stats KotlinInstanceRewriter::transform(
    ConcurrentMap<DexFieldRef*,
                  std::set<std::pair<IRInstruction*, DexMethod*>>>&
        concurrent_instance_map) {
  // Among the Kotlin classes which sets INSTANCE and whose use is cleary
  // defined, select singles use INSTANCEs. Remove the INSTANCE initialization
  // in <clinit> and replace the SGET INSTANCE with a new INSTANCE.
  std::vector<DexFieldRef*> fields_to_rewrite;
  KotlinInstanceRewriter::Stats stats = {};
  for (const auto& it : concurrent_instance_map) {
    if (it.second.size() > max_no_of_instance) {
      continue;
    }
    const std::set<std::pair<IRInstruction*, DexMethod*>>& insns = it.second;
    if (insns.empty()) {
      continue;
    }
    auto field = it.first;
    fields_to_rewrite.push_back(field);
  }
  std::sort(fields_to_rewrite.begin(), fields_to_rewrite.end(),
            compare_dexfields);

  for (auto* field : fields_to_rewrite) {
    stats.kotlin_instances_with_single_use++;
    // Remove instance from cls
    auto* cls = type_class(field->get_class());
    auto dmethods = cls->get_dmethods();
    for (auto* meth : dmethods) {
      if (method::is_clinit(meth)) {
        cfg::ScopedCFG cfg(meth->get_code());
        cfg::CFGMutation m(*cfg);
        TRACE(KOTLIN_INSTANCE, 5, "%s <clinit> before\n%s", SHOW(cls),
              SHOW(*cfg));
        auto iterable = cfg::InstructionIterable(*cfg);
        for (auto insn_it = iterable.begin(); insn_it != iterable.end();
             insn_it++) {
          auto insn = insn_it->insn;
          if (!opcode::is_an_sput(insn->opcode()) ||
              insn->get_field() != field) {
            continue;
          }

          m.remove(insn_it);
          stats.kotlin_instance_fields_removed++;
        }
        m.flush();
        TRACE(KOTLIN_INSTANCE, 5, "%s <clinit> after\n%s", SHOW(cls),
              SHOW(*cfg));
        break;
      }
    }

    // Convert INSTANCE read to new instance creation
    for (auto& method_it : concurrent_instance_map.find(field)->second) {
      auto* meth = method_it.second;
      cfg::ScopedCFG cfg(meth->get_code());
      cfg::CFGMutation m(*cfg);
      TRACE(KOTLIN_INSTANCE, 5, "%s before\n%s", SHOW(meth), SHOW(*cfg));
      DexMethodRef* init = DexMethod::get_method(
          cls->get_type(), DexString::make_string("<init>"),
          DexProto::make_proto(type::_void(), DexTypeList::make_type_list({})));
      always_assert(init);
      // Make this constructor publcic
      set_public(init->as_def());
      auto iterable = cfg::InstructionIterable(*cfg);
      for (auto insn_it = iterable.begin(); insn_it != iterable.end();
           insn_it++) {
        auto insn = insn_it->insn;
        if (!opcode::is_an_sget(insn->opcode()) || insn->get_field() != field) {
          continue;
        }
        auto move_result_it = cfg->move_result_of(insn_it);
        IRInstruction* new_isn = new IRInstruction(OPCODE_NEW_INSTANCE);
        new_isn->set_type(cls->get_type());
        IRInstruction* mov_result =
            new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        mov_result->set_dest(move_result_it->insn->dest());
        IRInstruction* init_isn = new IRInstruction(OPCODE_INVOKE_DIRECT);
        init_isn->set_method(init)->set_srcs_size(1)->set_src(
            0, move_result_it->insn->dest());
        m.replace(insn_it, {new_isn, mov_result, init_isn});
        m.remove(move_result_it);
        stats.kotlin_new_inserted++;
      }
      m.flush();
      TRACE(KOTLIN_INSTANCE, 5, "%s after\n%s", SHOW(meth), SHOW(*cfg));
    }
    cls->remove_field(resolve_field(field));
  }
  return stats;
}

void KotlinInstanceRewriter::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("kotlin_new_instance", kotlin_new_instance);
  mgr.incr_metric("kotlin_new_instance_which_escapes",
                  kotlin_new_instance_which_escapes);
  mgr.incr_metric("kotlin_instances_with_single_use",
                  kotlin_instances_with_single_use);
  mgr.incr_metric("kotlin_instance_fields_removed",
                  kotlin_instance_fields_removed);
  mgr.incr_metric("kotlin_new_inserted", kotlin_new_inserted);

  TRACE(KOTLIN_INSTANCE, 1, "kotlin_new_instance = %zu", kotlin_new_instance);
  TRACE(KOTLIN_INSTANCE, 1, "kotlin_new_instance_which_escapes = %zu",
        kotlin_new_instance_which_escapes);
  TRACE(KOTLIN_INSTANCE, 1, "kotlin_instances_with_single_use = %zu",
        kotlin_instances_with_single_use);
  TRACE(KOTLIN_INSTANCE, 1, "kotlin_instance_fields_removed = %zu",
        kotlin_instance_fields_removed);
  TRACE(KOTLIN_INSTANCE, 1, "kotlin_new_inserted = %zu", kotlin_new_inserted);
}
