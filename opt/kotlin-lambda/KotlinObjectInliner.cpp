/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinObjectInliner.h"

#include "CFGMutation.h"
#include "ConcurrentContainers.h"
#include "Creators.h"
#include "IRCode.h"
#include "LiveRange.h"
#include "Mutators.h"
#include "PassManager.h"
#include "Show.h"
#include "TypeInference.h"
#include "Walkers.h"

namespace {
void dump_cls(DexClass* cls) {
  TRACE(KOTLIN_OBJ_INLINE, 5, "Class %s", SHOW(cls));
  std::vector<DexMethod*> methods = cls->get_all_methods();
  std::vector<DexField*> fields = cls->get_all_fields();
  for (auto* v : fields) {
    TRACE(KOTLIN_OBJ_INLINE, 5, "Field %s", SHOW(v));
  }
  for (auto* v : methods) {
    TRACE(KOTLIN_OBJ_INLINE, 5, "Method %s", SHOW(v));
    if (v->get_code()) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "%s", SHOW(v->get_code()));
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
  auto code = method->get_code();
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  auto iterable = InstructionIterable(cfg.get_param_instructions());
  if (iterable.empty() && is_static(method)) {
    return false;
  }
  always_assert(!iterable.empty());
  live_range::MoveAwareChains chains(cfg);
  std::unordered_set<live_range::Use> first_load_param_uses;

  auto first_load_param = iterable.begin()->insn;
  first_load_param_uses =
      std::move(chains.get_def_use_chains()[first_load_param]);
  if (first_load_param_uses.empty()) {
    // "this" is not used.
    return false;
  }

  for (auto use : first_load_param_uses) {
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

  TRACE(KOTLIN_OBJ_INLINE, 1, "the init is %s!\n", SHOW(meth));
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
    auto insn = it->insn;
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

// Check if CLS is a companion object
// Companion object is:
// 1. Inner Object class
// 2. Will not have <clinit>
// 3. Will not have any fields (val and var is lifted to outer class if there is
// any).
// 4. Outer (or parent) class may have <clinit> which create instance of this
// (parent has sfield of inner class)
// 5. CLS is final and extends J_L_O
// If this is a candidate, return outer class. Return nullptr otherwise.
DexClass* candidate_for_companion_inlining(DexClass* cls) {
  if (root(cls) || !can_rename(cls) || !can_delete(cls) ||
      cls->rstate.is_referenced_by_resource_xml() || cls->is_external()) {
    return nullptr;
  }
  if (!is_final(cls) || !cls->get_ifields().empty() ||
      !cls->get_interfaces()->empty() || cls->get_clinit() ||
      !cls->get_sfields().empty() ||
      cls->get_super_class() != type::java_lang_Object()) {
    if (boost::ends_with(cls->get_name()->str(), "$Companion;")) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "Rejected $Companion cls = %s", SHOW(cls));
    }
    return nullptr;
  }
  DexClass* outer_cls = get_outer_class(cls);

  // Currently, we don't support companion class is in an abstract class.
  if (!outer_cls || is_abstract(outer_cls)) {
    return nullptr;
  }

  bool found = false;
  for (auto* sfield : outer_cls->get_sfields()) {
    if (sfield->get_type() == cls->get_type()) {
      if (found) {
        // Expect only one sfield in outer class to hold companion object
        // instance
        TRACE(KOTLIN_OBJ_INLINE, 5, "Rejected cls = %s", SHOW(cls));
        return nullptr;
      }
      found = true;
    }
  }

  for (auto meth : cls->get_vmethods()) {
    if (meth->rstate.no_optimizations() || !is_final(meth) ||
        !meth->get_code() || uses_this(meth)) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "Failed due to method = %s", SHOW(meth));
      return nullptr;
    }
  }

  for (auto meth : cls->get_dmethods()) {
    if (method::is_clinit(meth)) {
      return nullptr;
    }
    if (method::is_init(meth)) {
      if (!is_valid_init(meth)) {
        TRACE(KOTLIN_OBJ_INLINE, 5, "invalid init = %s", SHOW(meth));
        return nullptr;
      }
    } else if (meth->rstate.no_optimizations() || !meth->get_code() ||
               uses_this(meth)) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "Failed due to method = %s", SHOW(meth));
      return nullptr;
    }
  }

  return outer_cls->rstate.is_referenced_by_resource_xml() ? nullptr
                                                           : outer_cls;
}

void relocate(DexClass* comp_cls,
              DexClass* outer_cls,
              std::unordered_set<DexMethodRef*>& relocated_methods) {
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

  TRACE(KOTLIN_OBJ_INLINE, 5, "Before Relocating, the comp_cls is:");
  dump_cls(comp_cls);
  TRACE(KOTLIN_OBJ_INLINE, 5, "Before Relocating, the outer_cls is:");
  dump_cls(outer_cls);

  // Remove the <init> in the <clinit>
  if (outer_cls->get_clinit()) {
    auto* clinit_method = outer_cls->get_clinit();
    auto code = clinit_method->get_code();
    auto& clinit_cfg = code->cfg();
    cfg::CFGMutation m(clinit_cfg);
    auto iterable = cfg::InstructionIterable(clinit_cfg);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;
      if (opcode::is_new_instance(insn->opcode())) {
        auto* host_typ = insn->get_type();
        if (host_typ == comp_cls->get_type()) {
          auto mov_result_it = clinit_cfg.move_result_of(it);
          auto init_null = new IRInstruction(OPCODE_CONST);
          init_null->set_literal(0);
          init_null->set_dest(mov_result_it->insn->dest());
          m.replace(it, {init_null});
          TRACE(KOTLIN_OBJ_INLINE, 5, "Remove insn %s", SHOW(insn));
        }
      }
      if (opcode::is_an_invoke(insn->opcode()) &&
          method::is_init(insn->get_method())) {
        auto* host_typ = insn->get_method()->get_class();
        if (host_typ == comp_cls->get_type()) {
          m.remove(it);
          TRACE(KOTLIN_OBJ_INLINE, 5, "Remove insn %s", SHOW(insn));
        }
      }
      if (opcode::is_an_sput(insn->opcode()) && insn->get_field() == field) {
        TRACE(KOTLIN_OBJ_INLINE, 5, "Remove insn %s", SHOW(insn));
        m.remove(it);
      }
    }
    m.flush();
  }

  if (field) {
    TRACE(KOTLIN_OBJ_INLINE, 5, "Remove field %s", SHOW(field));
    outer_cls->remove_field(field);
  }

  // Relocate methods from comp_cls to outer_cls
  std::vector<DexMethod*> methods = comp_cls->get_all_methods();
  for (auto* method : methods) {
    if (method::is_init(method)) {
      continue;
    }
    TRACE(KOTLIN_OBJ_INLINE,
          5,
          "Relocating :(%s)  %s -> %s",
          SHOW(method),
          SHOW(comp_cls),
          SHOW(outer_cls));
    make_static_and_relocate_method(method, outer_cls->get_type());
    relocated_methods.insert(method);
  }

  TRACE(KOTLIN_OBJ_INLINE, 5, "After relocating, the comp class is:");
  dump_cls(comp_cls);
  TRACE(KOTLIN_OBJ_INLINE, 5, "After relocating, the outer class is:");
  dump_cls(outer_cls);
}

bool is_def_trackable(IRInstruction* insn,
                      const DexClass* from,
                      live_range::MoveAwareChains& move_aware_chains) {
  auto du_chains_move_aware = move_aware_chains.get_def_use_chains();
  if (!du_chains_move_aware.count(insn)) {
    // No use insns.
    return true;
  }
  const auto& use_set = du_chains_move_aware.at(insn);
  for (const auto& p : use_set) {
    auto use_insn = p.insn;
    auto use_index = p.src_index;
    switch (use_insn->opcode()) {
    case OPCODE_MOVE_OBJECT:
      break;
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_VIRTUAL:
      if (use_index != 0 ||
          type_class(use_insn->get_method()->get_class()) != from) {
        TRACE(KOTLIN_OBJ_INLINE,
              2,
              "Adding cls %s to bad list due to insn %s",
              SHOW(from),
              SHOW(use_insn));
        return false;
      }
      break;
    default:
      TRACE(KOTLIN_OBJ_INLINE,
            2,
            "Adding cls %s to bad list due to insn %s",
            SHOW(from),
            SHOW(use_insn));
      return false;
    }
  }
  return true;
}

} // namespace

void KotlinObjectInliner::run_pass(DexStoresVector& stores,
                                   ConfigFiles&,
                                   PassManager& mgr) {

  const auto scope = build_class_scope(stores);

  ConcurrentMap<DexClass*, DexClass*> map;
  ConcurrentSet<DexClass*> bad;
  std::unordered_map<DexClass*, unsigned> outer_cls_count;
  std::unordered_set<DexType*> do_not_inline_set;
  Stats stats;
  for (auto& p : m_do_not_inline_list) {
    auto* do_not_inline_cls = DexType::get_type(p);
    if (do_not_inline_cls) {
      TRACE(KOTLIN_OBJ_INLINE,
            2,
            "do_not_inline_cls  : %s",
            SHOW(do_not_inline_cls));
      do_not_inline_set.insert(do_not_inline_cls);
    }
  }

  // Collect candidates
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (do_not_inline_set.count(cls->get_type())) {
      return;
    }
    auto outer_cls = candidate_for_companion_inlining(cls);
    if (outer_cls && !do_not_inline_set.count(outer_cls->get_type())) {
      // This is a candidate for inlining
      map.insert(std::make_pair(cls, outer_cls));
      TRACE(KOTLIN_OBJ_INLINE, 2, "Candidate cls : %s", SHOW(cls));
    }
  });
  stats.kotlin_candidate_companion_objects = map.size();

  for (auto& iter : map) {
    outer_cls_count[iter.second]++;
  }

  for (auto iter : map) {
    // We have multiple companion objects. But in each class, there is at most 1
    // companion object.
    if (outer_cls_count.find(iter.second)->second != 1) {
      bad.insert(iter.first);
    }
  }

  // Filter out any instance whose use is not tracktable
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (!code) {
      return;
    }

    // we cannot relocate returning companion obejct.
    auto* rtype = type_class(method->get_proto()->get_rtype());
    if (rtype && map.count(rtype)) {
      bad.insert(rtype);
      TRACE(KOTLIN_OBJ_INLINE,
            2,
            "Method %s returns companion object %s",
            SHOW(method),
            SHOW(rtype));
    }

    always_assert(code->editable_cfg_built());
    auto& cfg = code->cfg();
    auto iterable = cfg::InstructionIterable(cfg);
    live_range::MoveAwareChains move_aware_chains(cfg);
    type_inference::TypeInference type_inference(cfg);
    type_inference.run(method);
    auto& type_environments = type_inference.get_type_environments();

    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;
      switch (insn->opcode()) {
      case OPCODE_SPUT_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if (!from || !map.count(from) || bad.count(from)) {
          break;
        }
        // Should only be set from parent's <clinit>
        // Otherwise add it to bad list.
        if (method::is_clinit(method) &&
            type_class(method->get_class()) == map.find(from)->second) {
          break;
        }
        bad.insert(from);
        break;
      }

      // If there is any instance field, add it to bad list.
      case OPCODE_IPUT_OBJECT:
      case OPCODE_IGET_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if (!from || !map.count(from) || bad.count(from)) {
          break;
        }
        bad.insert(from);
        break;
      }

      case OPCODE_SGET_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if (!from || !map.count(from) || bad.count(from)) {
          break;
        }
        // Check we can track the uses of the Companion object instance.
        // i.e. Companion object is only used to invoke methods
        if (!is_def_trackable(insn, from, move_aware_chains)) {
          bad.insert(from);
        }
        break;
      }

      case OPCODE_INSTANCE_OF:
      case OPCODE_NEW_INSTANCE: {
        auto* from = type_class(insn->get_type());
        if (!from || !map.count(from) || bad.count(from)) {
          break;
        }
        if (method::is_clinit(method) &&
            type_class(method->get_class()) == map.find(from)->second) {
          break;
        }
        bad.insert(from);
        break;
      }

      case OPCODE_INVOKE_DIRECT: {
        auto* from = type_class(insn->get_method()->get_class());
        if (!method::is_init(insn->get_method()) || !from || !map.count(from) ||
            bad.count(from)) {
          break;
        }
        if ((type_class(method->get_class()) == from &&
             method::is_init(method)) ||
            ((type_class(method->get_class()) == map.find(from)->second) &&
             method::is_clinit(method))) {
          break;
        }
        bad.insert(from);
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
        if (!from || !map.count(from) || bad.count(from)) {
          break;
        }
        bad.insert(from);
        TRACE(KOTLIN_OBJ_INLINE,
              2,
              "Adding cls %s to bad list due to insn %s",
              SHOW(from),
              SHOW(insn));
        break;
      }
      default:
        if (insn->has_type()) {
          auto* from = type_class(insn->get_type());
          if (!from || !map.count(from) || bad.count(from)) {
            break;
          }
          bad.insert(from);
          TRACE(KOTLIN_OBJ_INLINE,
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
  stats.kotlin_untrackable_companion_objects = bad.size();

  // Inline objects in candidate to maped class
  std::unordered_set<DexMethodRef*> relocated_methods;
  for (auto& p : map) {
    auto* comp_cls = p.first;
    auto* outer_cls = p.second;
    if (bad.count(comp_cls)) {
      continue;
    }
    TRACE(KOTLIN_OBJ_INLINE,
          2,
          "Relocate : %s -> %s",
          SHOW(comp_cls),
          SHOW(outer_cls));
    relocate(comp_cls, outer_cls, relocated_methods);
    stats.kotlin_companion_objects_inlined++;
  }

  // Fix virtual call arguments
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code == nullptr) {
      return;
    }
    bool changed = false;
    auto& cfg = method->get_code()->cfg();
    cfg::CFGMutation m(cfg);
    live_range::MoveAwareChains move_aware_chains(cfg);
    auto iterable = cfg::InstructionIterable(cfg);

    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;
      if (insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
          insn->opcode() == OPCODE_INVOKE_DIRECT) {
        if (!relocated_methods.count(insn->get_method())) {
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
      TRACE(KOTLIN_OBJ_INLINE, 5, "After : %s\n", SHOW(method));
      TRACE(KOTLIN_OBJ_INLINE, 5, "%s\n", SHOW(cfg));
    }
  });

  stats.report(mgr);
}

void KotlinObjectInliner::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("kotlin_candidate_companion_objects",
                  kotlin_candidate_companion_objects);
  mgr.incr_metric("kotlin_untrackable_companion_objects",
                  kotlin_untrackable_companion_objects);
  mgr.incr_metric("kotlin_companion_objects_inlined",
                  kotlin_companion_objects_inlined);
  TRACE(KOTLIN_OBJ_INLINE, 2, "KotlinObjectInliner Stats:");
  TRACE(KOTLIN_OBJ_INLINE,
        2,
        "kotlin_candidate_companion_objects = %zu",
        kotlin_candidate_companion_objects);
  TRACE(KOTLIN_OBJ_INLINE,
        2,
        "kotlin_untrackable_companion_objects = %zu",
        kotlin_untrackable_companion_objects);
  TRACE(KOTLIN_OBJ_INLINE,
        2,
        "kotlin_companion_objects_inlined = %zu",
        kotlin_companion_objects_inlined);
}

static KotlinObjectInliner s_pass;
