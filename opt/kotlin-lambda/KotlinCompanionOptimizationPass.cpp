/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinCompanionOptimizationPass.h"

#include "AtomicStatCounter.h"
#include "CFGMutation.h"
#include "ConcurrentContainers.h"
#include "Creators.h"
#include "DeterministicContainers.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "LiveRange.h"
#include "Mutators.h"
#include "PassManager.h"
#include "Show.h"
#include "TypeInference.h"
#include "Walkers.h"

namespace {
void dump_cls(DexClass* cls) {
  TRACE(KOTLIN_COMPANION, 5, "Class %s", SHOW(cls));
  std::vector<DexMethod*> methods = cls->get_all_methods();
  std::vector<DexField*> fields = cls->get_all_fields();
  for (auto* v : fields) {
    TRACE(KOTLIN_COMPANION, 5, "Field %s", SHOW(v));
  }
  for (auto* v : methods) {
    TRACE(KOTLIN_COMPANION, 5, "Method %s", SHOW(v));
    if (v->get_code() != nullptr) {
      TRACE(KOTLIN_COMPANION, 5, "%s", SHOW(v->get_code()));
    }
  }
}

// check if CLS is an inner class and return the outer class. Return nullptr if
// this is not an inner class.
DexClass* get_outer_class(const DexClass* cls) {
  const auto cls_name = cls->get_name()->str();
  auto cash_idx = cls_name.find_last_of('$');
  if (cash_idx == std::string::npos) {
    // this is not an inner class
    return nullptr;
  }
  auto slash_idx = cls_name.find_last_of('/');
  if (slash_idx == std::string::npos || slash_idx < cash_idx) {
    // there's a $ in the class name
    const std::string& outer_name = cls_name.substr(0, cash_idx) + ';';
    DexType* outer = DexType::get_type(outer_name);
    if (outer == nullptr) {
      return nullptr;
    }
    DexClass* outer_cls = type_class(outer);
    if (outer_cls == nullptr || outer_cls->is_external()) {
      return nullptr;
    }
    return outer_cls;
  }
  return nullptr;
}

// Check if the method uses the first argument (i.e this pointer).
// if strict == true, any use of this_reg will result in returning true.
// if strict == false, if his_reg is used just to invoke virtual
// methods from the same class, this will not be considered a use.
bool uses_this(const DexMethod* method, bool strict = false) {
  const auto* code = method->get_code();
  always_assert(code->cfg_built());
  const auto& cfg = code->cfg();
  auto iterable = InstructionIterable(cfg.get_param_instructions());
  if (iterable.empty() && is_static(method)) {
    return false;
  }
  always_assert(!iterable.empty());
  live_range::MoveAwareChains chains(cfg);
  live_range::Uses first_load_param_uses;

  auto* first_load_param = iterable.begin()->insn;
  first_load_param_uses =
      std::move(chains.get_def_use_chains()[first_load_param]);
  if (first_load_param_uses.empty()) {
    // "this" is not used.
    return false;
  }

  for (auto use : UnorderedIterable(first_load_param_uses)) {
    if (!strict &&
        (use.insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
         use.insn->opcode() == OPCODE_INVOKE_DIRECT) &&
        use.insn->get_method()->get_class() == method->get_class()) {
      continue;
    }
    return true;
  }

  return false;
}

// Make method static (if necessary) and relocate to TO_TYPE
void make_static_and_relocate_method(DexMethod* method, DexType* to_type) {
  if (!is_static(method)) {
    mutators::make_static(method,
                          uses_this(method, true) ? mutators::KeepThis::Yes
                                                  : mutators::KeepThis::No);
  }
  change_visibility(method, to_type);
  relocate_method(method, to_type);
}

// \returns true if \p meth meets kotlin companion class <init> request.
// \returns false otherwise.
bool is_valid_init(DexMethod* meth) {
  if (!method::is_init(meth)) {
    return false;
  }
  DexType* return_type = meth->get_proto()->get_rtype();
  DexTypeList* args = meth->get_proto()->get_args();
  if (!type::is_void(return_type) || args->size() > 1) {
    return false;
  }

  TRACE(KOTLIN_COMPANION, 1, "the init is %s!\n", SHOW(meth));
  // ()V
  if (args->empty()) {
    return true;
  }
  // (arg1)V
  // invoke-direct cls.<init>:()V or
  // invoke-direct Ljava/lang/Object;.<init>:()V
  // return-void
  auto& init_cfg = meth->get_code()->cfg();
  auto iterable = cfg::InstructionIterable(init_cfg);
  for (auto it = iterable.begin(); it != iterable.end(); it++) {
    auto* insn = it->insn;
    switch (insn->opcode()) {
    case OPCODE_MOVE_OBJECT:
    case OPCODE_RETURN_VOID:
    case IOPCODE_LOAD_PARAM_OBJECT:
      break;
    case OPCODE_INVOKE_DIRECT: {
      auto* cls = type_class(insn->get_method()->get_class());
      if (!method::is_init(insn->get_method()) ||
          (cls != type_class(meth->get_class()) &&
           cls->get_type() != type::java_lang_Object())) {
        return false;
      }
      break;
    }
    default:
      return false;
    }
  }
  return true;
}

enum class RejectionReason {
  kAccepted,
  kRootedOrExternal,
  kNotFinal,
  kHasIfields,
  kHasInterfaces,
  kHasClinit,
  kHasSfields,
  kNonObjectSuper,
  kNoOuterClass,
  kAbstractOuter,
  kMultipleCompanionSfields,
  kInvalidInit,
  kMethodUsesThis,
};

// Check if CLS is a companion object
// Companion object is:
// 1. Inner Object class
// 2. Will not have <clinit>
// 3. Will not have any fields (val and var is lifted to outer class if there is
// any).
// 4. Outer (or parent) class may have <clinit> which create instance of this
// (parent has sfield of inner class)
// 5. CLS is final and extends J_L_O
// If this is a candidate, return outer class via OUT and kAccepted.
// Otherwise return the rejection reason.
std::pair<RejectionReason, DexClass*> candidate_for_companion_relocation(
    DexClass* cls) {
  if (root(cls) || !can_rename(cls) || !can_delete(cls) ||
      cls->rstate.is_referenced_by_resource_xml() || cls->is_external()) {
    return {RejectionReason::kRootedOrExternal, nullptr};
  }
  if (!is_final(cls)) {
    return {RejectionReason::kNotFinal, nullptr};
  }
  if (!cls->get_ifields().empty()) {
    return {RejectionReason::kHasIfields, nullptr};
  }
  if (!cls->get_interfaces()->empty()) {
    return {RejectionReason::kHasInterfaces, nullptr};
  }
  if (cls->get_clinit() != nullptr) {
    return {RejectionReason::kHasClinit, nullptr};
  }
  if (!cls->get_sfields().empty()) {
    return {RejectionReason::kHasSfields, nullptr};
  }
  if (cls->get_super_class() != type::java_lang_Object()) {
    return {RejectionReason::kNonObjectSuper, nullptr};
  }
  DexClass* outer_cls = get_outer_class(cls);

  // Currently, we don't support companion class is in an abstract class.
  if (outer_cls == nullptr) {
    return {RejectionReason::kNoOuterClass, nullptr};
  }
  if (is_abstract(outer_cls)) {
    return {RejectionReason::kAbstractOuter, nullptr};
  }

  bool found = false;
  for (auto* sfield : outer_cls->get_sfields()) {
    if (sfield->get_type() == cls->get_type()) {
      if (found) {
        // Expect only one sfield in outer class to hold companion object
        // instance
        TRACE(KOTLIN_COMPANION, 5, "Rejected cls = %s", SHOW(cls));
        return {RejectionReason::kMultipleCompanionSfields, nullptr};
      }
      found = true;
    }
  }

  for (auto* meth : cls->get_vmethods()) {
    if (meth->rstate.no_optimizations() || !is_final(meth) ||
        (meth->get_code() == nullptr) || uses_this(meth)) {
      TRACE(KOTLIN_COMPANION, 5, "Failed due to method = %s", SHOW(meth));
      return {RejectionReason::kMethodUsesThis, nullptr};
    }
  }

  for (auto* meth : cls->get_dmethods()) {
    if (method::is_clinit(meth)) {
      return {RejectionReason::kHasClinit, nullptr};
    }
    if (method::is_init(meth)) {
      if (!is_valid_init(meth)) {
        TRACE(KOTLIN_COMPANION, 5, "invalid init = %s", SHOW(meth));
        return {RejectionReason::kInvalidInit, nullptr};
      }
    } else if (meth->rstate.no_optimizations() ||
               (meth->get_code() == nullptr) || uses_this(meth)) {
      TRACE(KOTLIN_COMPANION, 5, "Failed due to method = %s", SHOW(meth));
      return {RejectionReason::kMethodUsesThis, nullptr};
    }
  }

  if (outer_cls->rstate.is_referenced_by_resource_xml()) {
    return {RejectionReason::kRootedOrExternal, nullptr};
  }
  return {RejectionReason::kAccepted, outer_cls};
}

void relocate(DexClass* comp_cls,
              DexClass* outer_cls,
              UnorderedSet<DexMethodRef*>& relocated_methods) {
  // There should not be any sfields or ifieds in companion object class.
  always_assert(comp_cls->get_sfields().empty());
  always_assert(comp_cls->get_ifields().empty());

  // Remove the instance from outer_cls class
  DexField* field = nullptr;
  for (auto* sfield : outer_cls->get_sfields()) {
    if (type_class(sfield->get_type()) == comp_cls) {
      always_assert(field == nullptr);
      field = sfield;
    }
  }

  TRACE(KOTLIN_COMPANION, 5, "Before Relocating, the comp_cls is:");
  dump_cls(comp_cls);
  TRACE(KOTLIN_COMPANION, 5, "Before Relocating, the outer_cls is:");
  dump_cls(outer_cls);

  // Remove the <init> in the <clinit>
  if (outer_cls->get_clinit() != nullptr) {
    auto* clinit_method = outer_cls->get_clinit();
    auto* code = clinit_method->get_code();
    auto& clinit_cfg = code->cfg();
    cfg::CFGMutation m(clinit_cfg);
    auto iterable = cfg::InstructionIterable(clinit_cfg);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto* insn = it->insn;
      if (opcode::is_new_instance(insn->opcode())) {
        auto* host_typ = insn->get_type();
        if (host_typ == comp_cls->get_type()) {
          auto mov_result_it = clinit_cfg.move_result_of(it);
          auto* init_null = new IRInstruction(OPCODE_CONST);
          init_null->set_literal(0);
          init_null->set_dest(mov_result_it->insn->dest());
          m.replace(it, {init_null});
          TRACE(KOTLIN_COMPANION, 5, "Remove insn %s", SHOW(insn));
        }
      }
      if (opcode::is_an_invoke(insn->opcode()) &&
          method::is_init(insn->get_method())) {
        auto* host_typ = insn->get_method()->get_class();
        if (host_typ == comp_cls->get_type()) {
          m.remove(it);
          TRACE(KOTLIN_COMPANION, 5, "Remove insn %s", SHOW(insn));
        }
      }
      if (opcode::is_an_sput(insn->opcode()) && insn->get_field() == field) {
        TRACE(KOTLIN_COMPANION, 5, "Remove insn %s", SHOW(insn));
        m.remove(it);
      }
    }
    m.flush();
  }

  if (field != nullptr) {
    TRACE(KOTLIN_COMPANION, 5, "Remove field %s", SHOW(field));
    outer_cls->remove_field(field);
  }

  // Relocate methods from comp_cls to outer_cls
  std::vector<DexMethod*> methods = comp_cls->get_all_methods();
  for (auto* method : methods) {
    if (method::is_init(method)) {
      continue;
    }
    TRACE(KOTLIN_COMPANION,
          5,
          "Relocating :(%s)  %s -> %s",
          SHOW(method),
          SHOW(comp_cls),
          SHOW(outer_cls));
    make_static_and_relocate_method(method, outer_cls->get_type());
    relocated_methods.insert(method);
  }

  TRACE(KOTLIN_COMPANION, 5, "After relocating, the comp class is:");
  dump_cls(comp_cls);
  TRACE(KOTLIN_COMPANION, 5, "After relocating, the outer class is:");
  dump_cls(outer_cls);
}

bool is_def_trackable(IRInstruction* insn,
                      const DexClass* from,
                      live_range::MoveAwareChains& move_aware_chains) {
  auto du_chains_move_aware = move_aware_chains.get_def_use_chains();
  if (du_chains_move_aware.count(insn) == 0u) {
    // No use insns.
    return true;
  }
  const auto& use_set = du_chains_move_aware.at(insn);
  for (const auto& p : UnorderedIterable(use_set)) {
    auto* use_insn = p.insn;
    auto use_index = p.src_index;
    switch (use_insn->opcode()) {
    case OPCODE_MOVE_OBJECT:
      break;
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_VIRTUAL:
      if (use_index != 0 ||
          type_class(use_insn->get_method()->get_class()) != from) {
        TRACE(KOTLIN_COMPANION,
              2,
              "Adding cls %s to bad list due to insn %s",
              SHOW(from),
              SHOW(use_insn));
        return false;
      }
      break;
    default:
      TRACE(KOTLIN_COMPANION,
            2,
            "Adding cls %s to bad list due to insn %s",
            SHOW(from),
            SHOW(use_insn));
      return false;
    }
  }
  return true;
}

// Per-reason rejection counts from structural candidate checks.
struct RejectionCounts {
  AtomicStatCounter<size_t> not_final{0};
  AtomicStatCounter<size_t> has_sfields{0};
  AtomicStatCounter<size_t> has_clinit{0};
  AtomicStatCounter<size_t> has_interfaces{0};
  AtomicStatCounter<size_t> has_ifields{0};
  AtomicStatCounter<size_t> non_object_super{0};
  AtomicStatCounter<size_t> no_outer_class{0};
  AtomicStatCounter<size_t> abstract_outer{0};
  AtomicStatCounter<size_t> invalid_init{0};
  AtomicStatCounter<size_t> method_uses_this{0};
};

// Phase 1: Structural candidate collection + duplicate outer class filtering.
// Fills `candidates` with companion->outer mappings that pass structural
// checks. Adds companions with duplicate outer classes to `rejected`. Populates
// `counts` with per-reason rejection tallies.
void collect_candidates(
    const Scope& scope,
    const UnorderedSet<DexType*>& do_not_relocate_set,
    InsertOnlyConcurrentMap<DexClass*, DexClass*>& candidates,
    ConcurrentSet<DexClass*>& rejected,
    RejectionCounts& counts) {
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (do_not_relocate_set.count(cls->get_type()) != 0u) {
      return;
    }
    auto [reason, outer_cls] = candidate_for_companion_relocation(cls);
    switch (reason) {
    case RejectionReason::kNotFinal:
      ++counts.not_final;
      return;
    case RejectionReason::kHasSfields:
      ++counts.has_sfields;
      return;
    case RejectionReason::kHasClinit:
      ++counts.has_clinit;
      return;
    case RejectionReason::kHasInterfaces:
      ++counts.has_interfaces;
      return;
    case RejectionReason::kHasIfields:
      ++counts.has_ifields;
      return;
    case RejectionReason::kNonObjectSuper:
      ++counts.non_object_super;
      return;
    case RejectionReason::kNoOuterClass:
      ++counts.no_outer_class;
      return;
    case RejectionReason::kAbstractOuter:
      ++counts.abstract_outer;
      return;
    case RejectionReason::kInvalidInit:
      ++counts.invalid_init;
      return;
    case RejectionReason::kMethodUsesThis:
      ++counts.method_uses_this;
      return;
    case RejectionReason::kRootedOrExternal:
    case RejectionReason::kMultipleCompanionSfields:
      return;
    case RejectionReason::kAccepted:
      break;
    }
    if (do_not_relocate_set.count(outer_cls->get_type()) == 0u) {
      // This is a candidate for relocation
      candidates.insert(std::make_pair(cls, outer_cls));
      TRACE(KOTLIN_COMPANION, 2, "Candidate cls : %s", SHOW(cls));
    }
  });

  // Filter out companions with duplicate outer classes.
  UnorderedMap<DexClass*, unsigned> outer_cls_count;
  for (auto& iter : UnorderedIterable(candidates)) {
    outer_cls_count[iter.second]++;
  }
  for (auto iter : UnorderedIterable(candidates)) {
    if (outer_cls_count.find(iter.second)->second != 1) {
      rejected.insert(iter.first);
    }
  }
}

// Phase 2: Full-program safety analysis.
// Scans all methods for unsafe uses of companion instances and adds them to
// `rejected`.
void filter_untrackable_usages(
    const Scope& scope,
    const InsertOnlyConcurrentMap<DexClass*, DexClass*>& candidates,
    ConcurrentSet<DexClass*>& rejected) {
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto* code = method->get_code();
    if (code == nullptr) {
      return;
    }

    // We cannot relocate returning companion object.
    auto* rtype = type_class(method->get_proto()->get_rtype());
    if ((rtype != nullptr) && (candidates.count(rtype) != 0u)) {
      rejected.insert(rtype);
      TRACE(KOTLIN_COMPANION,
            2,
            "Method %s returns companion object %s",
            SHOW(method),
            SHOW(rtype));
    }

    always_assert(code->cfg_built());
    auto& cfg = code->cfg();
    auto iterable = cfg::InstructionIterable(cfg);
    live_range::MoveAwareChains move_aware_chains(cfg);
    type_inference::TypeInference type_inference(cfg);
    type_inference.run(method);
    auto& type_environments = type_inference.get_type_environments();

    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto* insn = it->insn;
      switch (insn->opcode()) {
      case OPCODE_SPUT_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if ((from == nullptr) || (candidates.count(from) == 0u) ||
            (rejected.count(from) != 0u)) {
          break;
        }
        // Should only be set from parent's <clinit>
        // Otherwise add it to bad list.
        if (method::is_clinit(method) &&
            type_class(method->get_class()) == candidates.find(from)->second) {
          break;
        }
        rejected.insert(from);
        break;
      }

      // If there is any instance field, add it to bad list.
      case OPCODE_IPUT_OBJECT:
      case OPCODE_IGET_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if ((from == nullptr) || (candidates.count(from) == 0u) ||
            (rejected.count(from) != 0u)) {
          break;
        }
        rejected.insert(from);
        break;
      }

      case OPCODE_SGET_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if ((from == nullptr) || (candidates.count(from) == 0u) ||
            (rejected.count(from) != 0u)) {
          break;
        }
        // Check we can track the uses of the Companion object instance.
        // i.e. Companion object is only used to invoke methods
        if (!is_def_trackable(insn, from, move_aware_chains)) {
          rejected.insert(from);
        }
        break;
      }

      case OPCODE_INSTANCE_OF:
      case OPCODE_NEW_INSTANCE: {
        auto* from = type_class(insn->get_type());
        if ((from == nullptr) || (candidates.count(from) == 0u) ||
            (rejected.count(from) != 0u)) {
          break;
        }
        if (method::is_clinit(method) &&
            type_class(method->get_class()) == candidates.find(from)->second) {
          break;
        }
        rejected.insert(from);
        break;
      }

      case OPCODE_INVOKE_DIRECT: {
        auto* from = type_class(insn->get_method()->get_class());
        if (!method::is_init(insn->get_method()) || (from == nullptr) ||
            (candidates.count(from) == 0u) || (rejected.count(from) != 0u)) {
          break;
        }
        if ((type_class(method->get_class()) == from &&
             method::is_init(method)) ||
            ((type_class(method->get_class()) ==
              candidates.find(from)->second) &&
             method::is_clinit(method))) {
          break;
        }
        rejected.insert(from);
        break;
      }

      case OPCODE_APUT_OBJECT:
      case OPCODE_AGET_OBJECT: {
        auto& env = type_environments.at(insn);
        auto dex_type = env.get_dex_type(insn->src(0));
        if (!dex_type) {
          break;
        }
        DexClass* from;
        if (insn->opcode() == OPCODE_AGET_OBJECT) {
          if (!type::is_array(*dex_type)) {
            break;
          }
          from = type_class(type::get_array_component_type(*dex_type));
        } else {
          from = type_class(*dex_type);
        }
        // Currently, we don't supporting tracking companion object usage in
        // aget/aput_object. Instead, simply insert it into bad list.
        if ((from == nullptr) || (candidates.count(from) == 0u) ||
            (rejected.count(from) != 0u)) {
          break;
        }
        rejected.insert(from);
        TRACE(KOTLIN_COMPANION,
              2,
              "Adding cls %s to bad list due to insn %s",
              SHOW(from),
              SHOW(insn));
        break;
      }
      default:
        if (insn->has_type()) {
          auto* from = type_class(insn->get_type());
          if ((from == nullptr) || (candidates.count(from) == 0u) ||
              (rejected.count(from) != 0u)) {
            break;
          }
          rejected.insert(from);
          TRACE(KOTLIN_COMPANION,
                2,
                "Adding cls %s to bad list due to insn %s",
                SHOW(from),
                SHOW(insn));
          break;
        }
        break;
      }
    }
  });
}

// Phase 3: Relocate companion methods to outer classes.
// Returns the set of relocated method refs and the count of relocated
// companions.
std::pair<UnorderedSet<DexMethodRef*>, size_t> relocate_companions(
    const InsertOnlyConcurrentMap<DexClass*, DexClass*>& candidates,
    const ConcurrentSet<DexClass*>& rejected) {
  UnorderedSet<DexMethodRef*> relocated_methods;
  size_t relocated_count = 0;
  for (const auto& p : UnorderedIterable(candidates)) {
    auto* comp_cls = p.first;
    auto* outer_cls = p.second;
    if (rejected.count(comp_cls) != 0u) {
      continue;
    }
    TRACE(KOTLIN_COMPANION,
          2,
          "Relocate : %s -> %s",
          SHOW(comp_cls),
          SHOW(outer_cls));
    relocate(comp_cls, outer_cls, relocated_methods);
    relocated_count++;
  }
  return {std::move(relocated_methods), relocated_count};
}

// Phase 4: Rewrite call sites from invoke-virtual/direct to invoke-static.
void rewrite_call_sites(const Scope& scope,
                        const UnorderedSet<DexMethodRef*>& relocated_methods) {
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto* code = method->get_code();
    if (code == nullptr) {
      return;
    }
    bool changed = false;
    auto& cfg = method->get_code()->cfg();
    cfg::CFGMutation m(cfg);
    live_range::MoveAwareChains move_aware_chains(cfg);
    auto iterable = cfg::InstructionIterable(cfg);

    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto* insn = it->insn;
      if (insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
          insn->opcode() == OPCODE_INVOKE_DIRECT) {
        if (relocated_methods.count(insn->get_method()) == 0u) {
          continue;
        }
        // When the method in Companion object is relocated to outer class,
        // it is changed to dmethod, and "this_pointer", may be removed.
        // Therefore, we need to remove the first this_pointer if necessary.
        insn->set_opcode(OPCODE_INVOKE_STATIC);
        size_t arg_count = insn->get_method()->get_proto()->get_args()->size();
        auto nargs = insn->srcs_size();
        if (arg_count != nargs) {
          for (uint16_t i = 0; i < nargs - 1; i++) {
            insn->set_src(i, insn->src(i + 1));
          }
          insn->set_srcs_size(nargs - 1);
        }
        always_assert(arg_count == insn->srcs_size());
        changed = true;
      }
    }
    if (changed) {
      m.flush();
      TRACE(KOTLIN_COMPANION, 5, "After : %s\n", SHOW(method));
      TRACE(KOTLIN_COMPANION, 5, "%s\n", SHOW(cfg));
    }
  });
}

} // namespace

void KotlinCompanionOptimizationPass::run_pass(DexStoresVector& stores,
                                               ConfigFiles&,
                                               PassManager& mgr) {
  const auto scope = build_class_scope(stores);

  UnorderedSet<DexType*> do_not_relocate_set;
  for (auto& p : m_do_not_relocate_list) {
    auto* t = DexType::get_type(p);
    if (t != nullptr) {
      TRACE(KOTLIN_COMPANION, 2, "do_not_relocate_cls  : %s", SHOW(t));
      do_not_relocate_set.insert(t);
    }
  }

  InsertOnlyConcurrentMap<DexClass*, DexClass*> candidates;
  ConcurrentSet<DexClass*> rejected;

  // Phase 1: Collect structurally eligible companion objects.
  RejectionCounts counts;
  collect_candidates(scope, do_not_relocate_set, candidates, rejected, counts);

  // Phase 2: Filter out companions with untrackable usages.
  filter_untrackable_usages(scope, candidates, rejected);

  // Phase 3: Relocate accepted companion methods to outer classes.
  auto [relocated_methods, relocated_count] =
      relocate_companions(candidates, rejected);

  // Phase 4: Rewrite call sites to use static dispatch.
  rewrite_call_sites(scope, relocated_methods);

  // Report stats.
  Stats stats;
  stats.kotlin_candidate_companion_objects = candidates.size();
  stats.kotlin_untrackable_companion_objects = rejected.size();
  stats.kotlin_companion_objects_relocated = relocated_count;
  stats.kotlin_rejected_not_final = counts.not_final;
  stats.kotlin_rejected_has_sfields = counts.has_sfields;
  stats.kotlin_rejected_has_clinit = counts.has_clinit;
  stats.kotlin_rejected_has_interfaces = counts.has_interfaces;
  stats.kotlin_rejected_has_ifields = counts.has_ifields;
  stats.kotlin_rejected_non_object_super = counts.non_object_super;
  stats.kotlin_rejected_no_outer_class = counts.no_outer_class;
  stats.kotlin_rejected_abstract_outer = counts.abstract_outer;
  stats.kotlin_rejected_invalid_init = counts.invalid_init;
  stats.kotlin_rejected_method_uses_this = counts.method_uses_this;
  stats.report(mgr);
}

void KotlinCompanionOptimizationPass::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("kotlin_candidate_companion_objects",
                  kotlin_candidate_companion_objects);
  mgr.incr_metric("kotlin_untrackable_companion_objects",
                  kotlin_untrackable_companion_objects);
  mgr.incr_metric("kotlin_companion_objects_relocated",
                  kotlin_companion_objects_relocated);
  mgr.incr_metric("kotlin_rejected_not_final", kotlin_rejected_not_final);
  mgr.incr_metric("kotlin_rejected_has_sfields", kotlin_rejected_has_sfields);
  mgr.incr_metric("kotlin_rejected_has_clinit", kotlin_rejected_has_clinit);
  mgr.incr_metric("kotlin_rejected_has_interfaces",
                  kotlin_rejected_has_interfaces);
  mgr.incr_metric("kotlin_rejected_has_ifields", kotlin_rejected_has_ifields);
  mgr.incr_metric("kotlin_rejected_non_object_super",
                  kotlin_rejected_non_object_super);
  mgr.incr_metric("kotlin_rejected_no_outer_class",
                  kotlin_rejected_no_outer_class);
  mgr.incr_metric("kotlin_rejected_abstract_outer",
                  kotlin_rejected_abstract_outer);
  mgr.incr_metric("kotlin_rejected_invalid_init", kotlin_rejected_invalid_init);
  mgr.incr_metric("kotlin_rejected_method_uses_this",
                  kotlin_rejected_method_uses_this);
  TRACE(KOTLIN_COMPANION, 2, "KotlinCompanionOptimizationPass Stats:");
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_candidate_companion_objects = %zu",
        kotlin_candidate_companion_objects);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_untrackable_companion_objects = %zu",
        kotlin_untrackable_companion_objects);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_companion_objects_relocated = %zu",
        kotlin_companion_objects_relocated);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_not_final = %zu",
        kotlin_rejected_not_final);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_has_sfields = %zu",
        kotlin_rejected_has_sfields);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_has_clinit = %zu",
        kotlin_rejected_has_clinit);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_has_interfaces = %zu",
        kotlin_rejected_has_interfaces);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_has_ifields = %zu",
        kotlin_rejected_has_ifields);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_non_object_super = %zu",
        kotlin_rejected_non_object_super);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_no_outer_class = %zu",
        kotlin_rejected_no_outer_class);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_abstract_outer = %zu",
        kotlin_rejected_abstract_outer);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_invalid_init = %zu",
        kotlin_rejected_invalid_init);
  TRACE(KOTLIN_COMPANION,
        2,
        "kotlin_rejected_method_uses_this = %zu",
        kotlin_rejected_method_uses_this);
}

static KotlinCompanionOptimizationPass s_pass;
