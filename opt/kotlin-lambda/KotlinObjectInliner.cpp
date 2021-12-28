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
#include "ScopedCFG.h"
#include "Show.h"
#include "Walkers.h"

namespace {
void dump_cls(DexClass* cls) {
  if (traceEnabled(KOTLIN_OBJ_INLINE, 5)) {
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
}

// check if CLS is an inner class and return the outer class. Return nullptr if
// this is not an inner class.
DexClass* get_outer_class(const DexClass* cls) {
  const std::string& cls_name = cls->get_name()->str();
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

// Check if the method uses the first argument (or this pointer).
// if strict == true, any use of this_reg will result in returning true.
// if strict == false, if his_reg is used just to invoke virtual
// methods from the same class, this will not be considered a use.
bool uses_this(const DexMethod* method, bool strict = false) {
  auto code = method->get_code();
  auto iterable = InstructionIterable(code);
  auto it = iterable.begin();
  auto const this_load_insn = it->insn;
  if (this_load_insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT) {
    return false;
  }
  std::unordered_set<reg_t> this_reg_set;
  auto const this_reg = this_load_insn->dest();
  this_reg_set.insert(this_reg);

  for (const auto& mie : iterable) {
    auto insn = mie.insn;
    for (unsigned i = 0; i < insn->srcs_size(); i++) {
      if (this_reg_set.count(insn->src(i))) {
        if (!strict && i == 0 && insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
            insn->get_method()->get_class() == method->get_class()) {
          continue;
        }
        return true;
      }
    }
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

// Check if CLS is a companion object
// Companion object is:
// 1. Inner Object class:
// 2. Will not have <clinit>
// 3. Will not have any direct methods other than constructors
// 4. Will not have any fields (TODO We could extend to support sfields)
// 5. Outer (or parent) class may have <clinit> which create instance of this
// (parent has sfield of inner class)
// 6. CLS is final and extends J_L_O
// If this is a candidate, return outer class. Return nullptr otherwise.
DexClass* candidate_for_companion_inlining(DexClass* cls) {

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

  if (!outer_cls || is_abstract(outer_cls)) {
    return nullptr;
  }

  bool found = false;
  for (auto* sfield : outer_cls->get_sfields()) {
    if (sfield->get_type() == cls->get_type()) {
      if (found) {
        // Expect only one sfield in outer class to hold companion object
        // instance
        TRACE(KOTLIN_OBJ_INLINE, 5, "3 Rejected cls = %s", SHOW(cls));
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
    if (method::is_init(meth) || method::is_clinit(meth)) {
      continue;
    }
    if (meth->rstate.no_optimizations() || !meth->get_code() ||
        uses_this(meth)) {
      TRACE(KOTLIN_OBJ_INLINE, 5, "Failed due to method = %s", SHOW(meth));
      return nullptr;
    }
  }
  return outer_cls;
}

void relocate(DexClass* from,
              DexClass* to,
              std::unordered_set<DexMethodRef*>& relocated_methods) {
  // Remove the instance in TO class
  DexField* field = nullptr;
  for (auto* sfield : to->get_sfields()) {
    if (type_class(sfield->get_type()) == from) {
      always_assert(field == nullptr);
      field = sfield;
    }
  }

  TRACE(KOTLIN_OBJ_INLINE, 5, "Relocating from:");
  dump_cls(from);
  TRACE(KOTLIN_OBJ_INLINE, 5, "Relocating to:");
  dump_cls(to);

  // Remove the <init> in the <clinit>
  if (to->get_clinit()) {
    auto* clinit_method = to->get_clinit();
    auto code = clinit_method->get_code();
    cfg::ScopedCFG clinit_cfg(code);
    cfg::CFGMutation m(*clinit_cfg);
    auto iterable = cfg::InstructionIterable(*clinit_cfg);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;
      if (opcode::is_new_instance(insn->opcode())) {
        auto* host_typ = insn->get_type();
        if (host_typ == from->get_type()) {
          auto mov_result_it = clinit_cfg->move_result_of(it);
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
        if (host_typ == from->get_type()) {
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
    to->remove_field(field);
  }
  // Relocate the methods from FROM to TO
  for (auto* method : from->get_vmethods()) {
    TRACE(KOTLIN_OBJ_INLINE,
          5,
          "Relocating :(%s)  %s -> %s",
          SHOW(method),
          SHOW(from),
          SHOW(to));
    make_static_and_relocate_method(method, to->get_type());
    relocated_methods.insert(method);
  }

  for (auto* method : from->get_dmethods()) {
    if (method::is_init(method) || method::is_clinit(method)) {
      continue;
    }
    TRACE(KOTLIN_OBJ_INLINE,
          5,
          "Relocating static method:(%s from) %s -> %s",
          SHOW(from),
          SHOW(method),
          SHOW(to));
    make_static_and_relocate_method(method, to->get_type());
    relocated_methods.insert(method);
  }
  for (auto* f : from->get_sfields()) {
    TRACE(KOTLIN_OBJ_INLINE,
          5,
          "Relocating static field:(%s from) %s -> %s",
          SHOW(from),
          SHOW(field),
          SHOW(to));
    relocate_field(f, to->get_type());
  }
  TRACE(KOTLIN_OBJ_INLINE, 5, "After relocating to:");
  dump_cls(to);
}

bool is_def_tractable(IRInstruction* insn,
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
    case OPCODE_INVOKE_STATIC: {
      // JVM static
      if (use_index != 0 ||
          type_class(use_insn->get_method()->get_class()) != from) {
        TRACE(KOTLIN_OBJ_INLINE,
              2,
              "Adding cls %s to bad list due to insn %s",
              SHOW(from),
              SHOW(use_insn));
        return false;
      }
    } break;
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_INTERFACE:
      // Check for likes of Ljava/lang/Object;.getClass:()Ljava/lang/Class;
      if (use_insn->get_method()->get_class() != from->get_type() ||
          use_index != 0) {
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
            "do_not_inlin_cls  : %s",
            SHOW(do_not_inline_cls));
      do_not_inline_set.insert(do_not_inline_cls);
    }
  }
  // Collect candidates
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (is_native(cls) || root(cls) || !can_rename(cls) || !can_delete(cls) ||
        cls->rstate.is_referenced_by_resource_xml() || cls->is_external() ||
        do_not_inline_set.count(cls->get_type())) {
      return;
    }
    auto outer_cls = candidate_for_companion_inlining(cls);
    if (outer_cls && !outer_cls->rstate.is_referenced_by_resource_xml() &&
        !do_not_inline_set.count(outer_cls->get_type())) {
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
    // We have mutiple companion objects.
    if (outer_cls_count.find(iter.second)->second != 1) {
      bad.insert(iter.first);
    }
  }
  // Filter out any instance whose use is not tractable
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (!code) {
      return;
    }

    // we cannot relocate returning companion obect.
    auto* rtype = type_class(method->get_proto()->get_rtype());
    if (rtype && map.count(rtype)) {
      bad.insert(rtype);
    }

    cfg::ScopedCFG cfg(code);
    auto iterable = cfg::InstructionIterable(*cfg);
    live_range::MoveAwareChains move_aware_chains(*cfg);

    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;
      switch (insn->opcode()) {
      case OPCODE_SPUT_OBJECT: {
        auto* from = type_class(insn->get_field()->get_type());
        if (!from || !map.count(from) || bad.count(from)) {
          break;
        }

        // Shold only be set from parent's <clinit>
        // Otherwise add it to bad list.
        if (method::is_clinit(method) &&
            type_class(method->get_class()) == map.find(from)->second) {
          break;
        }
        bad.insert(from);
        break;
      }

      // If there is any instance field, add it to bad
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
        if (!is_def_tractable(insn, from, move_aware_chains)) {
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
        TRACE(KOTLIN_OBJ_INLINE,
              2,
              "Adding cls %s to bad list due to insn %s",
              SHOW(from),
              SHOW(insn));
        break;
      }

      case OPCODE_CHECK_CAST: {
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
  //
  std::unordered_set<DexMethodRef*> relocated_methods;
  for (auto& p : map) {
    auto* from_cls = p.first;
    auto* to_cls = p.second;
    if (!bad.count(from_cls)) {
      TRACE(KOTLIN_OBJ_INLINE,
            2,
            "Relocate : %s -> %s",
            SHOW(from_cls),
            SHOW(to_cls));
      relocate(from_cls, to_cls, relocated_methods);
      stats.kotlin_companion_objects_inlined++;
    }
  }

  // Fix virtual call arguments
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code == nullptr) {
      return;
    }
    bool changed = false;
    cfg::ScopedCFG cfg(method->get_code());
    cfg::CFGMutation m(*cfg);
    live_range::MoveAwareChains move_aware_chains(*cfg);
    auto du_chains_move_aware = move_aware_chains.get_def_use_chains();
    auto iterable = cfg::InstructionIterable(*cfg);

    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;

      if (opcode::is_an_sput(insn->opcode())) {
        auto* from = type_class(insn->get_field()->get_type());
        if (!from || !map.count(from) || bad.count(from)) {
          continue;
        }
        auto mov_result_it = cfg->move_result_of(it);
        auto init_null = new IRInstruction(OPCODE_CONST);
        init_null->set_literal(0);
        init_null->set_dest(mov_result_it->insn->dest());
        m.replace(it, {init_null});
        changed = true;
      }
      if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
        if (!relocated_methods.count(insn->get_method())) {
          continue;
        }
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
      TRACE(KOTLIN_OBJ_INLINE, 5, "%s\n", SHOW(*cfg));
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
        "kotlin_candidate_companion_objects = %lu",
        kotlin_candidate_companion_objects);
  TRACE(KOTLIN_OBJ_INLINE,
        2,
        "kotlin_untrackable_companion_objects = %lu",
        kotlin_untrackable_companion_objects);
  TRACE(KOTLIN_OBJ_INLINE,
        2,
        "kotlin_companion_objects_inlined = %lu",
        kotlin_companion_objects_inlined);
}

static KotlinObjectInliner s_pass;
