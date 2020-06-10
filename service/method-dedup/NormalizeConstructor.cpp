/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NormalizeConstructor.h"

#include "DexClass.h"
#include "EditableCfgAdapter.h"
#include "MethodReference.h"
#include "Walkers.h"

namespace {
// clang-format off
/**
 * Summary example of a simple constructor whose parameters are only used to
 * initialize instance fields or only being passed to super constructor.
 * void <init>(B b, A a, D d, C c) {
 *  this.f2 = b;
 *  this.f1 = a;
 *  this.f3 = c;
 *  super.<init>(this, d);
 * }
 *
 * If the fields is in order f1, f2, f3.
 *
 * The summary of the constructor is
 *   super_ctor : super.<init>
 *   param_use_order is
 *      f1  <-  2
 *      f2  <-  1
 *      f3  <-  3
 *      super_ctor arg1 <- 4
 *
 * Any two bijective constructors like this in the same class are isomorphic.
 */
// clang-format on
struct ConstructorSummary {
  DexMethodRef* super_ctor{nullptr};
  // 1 to N.
  std::vector<uint32_t> param_use_order;

  ConstructorSummary(DexMethodRef* ctor, size_t num_fields) : super_ctor(ctor) {
    param_use_order.reserve(num_fields + ctor->get_proto()->get_args()->size());
  }
};

/**
 * @param ifields : Should include all the instance fields of the class.
 */
boost::optional<ConstructorSummary> summarize_constructor_logic(
    const std::vector<DexField*>& ifields, DexMethod* method) {
  if (root(method) || !is_constructor(method) || !method::is_init(method)) {
    return boost::none;
  }
  auto code = method->get_code();
  if (!code) {
    return boost::none;
  }
  IRInstruction* super_ctor_invocatoin = nullptr;
  std::unordered_map<uint32_t, uint32_t> reg_to_arg_id;
  std::unordered_map<DexFieldRef*, uint32_t> field_to_arg_id;
  uint32_t arg_index = 0;
  // TODO: Give up if there's exception handling.
  editable_cfg_adapter::iterate(code, [&](const MethodItemEntry& mie) {
    auto insn = mie.insn;
    auto opcode = insn->opcode();
    if (is_invoke_direct(opcode)) {
      auto ref = insn->get_method();
      if (!super_ctor_invocatoin && method::is_init(ref) &&
          ref->get_class() != method->get_class()) {
        super_ctor_invocatoin = insn;
        return editable_cfg_adapter::LoopExit::LOOP_CONTINUE;
      } else {
        super_ctor_invocatoin = nullptr;
        return editable_cfg_adapter::LoopExit::LOOP_BREAK;
      }
    } else if (is_return_void(opcode)) {
      return editable_cfg_adapter::LoopExit::LOOP_BREAK;
    } else if (is_iput(opcode)) {
      field_to_arg_id[insn->get_field()] = reg_to_arg_id[insn->src(0)];
      return editable_cfg_adapter::LoopExit::LOOP_CONTINUE;
    } else if (opcode::is_load_param(opcode)) {
      reg_to_arg_id[insn->dest()] = arg_index++;
      return editable_cfg_adapter::LoopExit::LOOP_CONTINUE;
    }
    // Not accept any other instruction.
    super_ctor_invocatoin = nullptr;
    return editable_cfg_adapter::LoopExit::LOOP_BREAK;
  });
  if (!super_ctor_invocatoin) {
    return boost::none;
  }
  if (field_to_arg_id.size() != ifields.size()) {
    return boost::none;
  }
  ConstructorSummary summary(super_ctor_invocatoin->get_method(),
                             ifields.size());
  std::unordered_set<uint32_t> used_args;
  for (auto field : ifields) {
    auto arg_id = field_to_arg_id[field];
    summary.param_use_order.push_back(arg_id);
    used_args.insert(arg_id);
  }
  redex_assert(reg_to_arg_id[super_ctor_invocatoin->src(0)] == 0);
  for (size_t id = 1; id < super_ctor_invocatoin->srcs_size(); id++) {
    auto arg_id = reg_to_arg_id[super_ctor_invocatoin->src(id)];
    summary.param_use_order.push_back(arg_id);
    used_args.insert(arg_id);
  }
  // Ensure bijection.
  if (used_args.size() != summary.param_use_order.size()) {
    return boost::none;
  }
  return summary;
}

using CtorSummaries = std::
    map<DexMethod*, boost::optional<ConstructorSummary>, dexmethods_comparator>;

/**
 * Choose the first one method that can be a representative, return nullptr if
 * no one is found.
 * When a representative is decided, its argument types may not be compatible to
 * other constructors', so its proto may need a change. The change should happen
 * at the end to avoid invalidating the key of the method set, so a new proto
 * record is needed for pending changes and method collision checking.
 */
DexMethod* get_representative(
    const CtorSummaries& methods,
    const std::vector<DexField*>& fields,
    const DexMethodRef* super_ctor,
    std::unordered_set<DexProto*>* pending_new_protos,
    std::unordered_map<DexMethodRef*, DexProto*>* global_pending_ctor_changes) {
  std::vector<DexType*> normalized_typelist;
  auto super_ctor_args = super_ctor->get_proto()->get_args();
  normalized_typelist.reserve(fields.size() + super_ctor_args->size());
  for (auto field : fields) {
    normalized_typelist.push_back(field->get_type());
  }
  normalized_typelist.insert(normalized_typelist.end(),
                             super_ctor_args->begin(), super_ctor_args->end());

  for (auto& pair : methods) {
    auto method = pair.first;
    auto& summary = pair.second;
    std::deque<DexType*> new_typelist;
    for (uint32_t index = 0; index < summary->param_use_order.size(); index++) {
      auto usesite = summary->param_use_order[index];
      new_typelist.push_back(normalized_typelist[usesite - 1]);
    }
    auto new_proto = DexProto::make_proto(
        type::_void(), DexTypeList::make_type_list(std::move(new_typelist)));
    if (pending_new_protos->count(new_proto)) {
      // The proto is pending for another constructor on this class.
      continue;
    }
    if (DexMethod::get_method(method->get_class(), method->get_name(),
                              new_proto)) {
      // The method with the new proto exists, it's impossible to change the
      // spec of the `method` to the new.
      continue;
    }
    pending_new_protos->insert(new_proto);
    if (new_proto != method->get_proto()) {
      (*global_pending_ctor_changes)[method] = new_proto;
    }
    return method;
  }
  return nullptr;
}

/**
 * set_src(i_old, src(j_new)) when old_param_use_order[i_old] ==
 * new_param_use_order[j_new],
 */
void reorder_callsite_args(const std::vector<uint32_t>& old_param_use_order,
                           const std::vector<uint32_t>& new_param_use_order,
                           IRInstruction* insn) {
  redex_assert(old_param_use_order.size() == new_param_use_order.size());
  std::unordered_map<uint32_t, uint32_t> old_field_id_to_param_id;
  for (uint32_t i_old = 1; i_old <= old_param_use_order.size(); i_old++) {
    old_field_id_to_param_id[old_param_use_order[i_old - 1]] = i_old;
  }
  auto old_srcs = insn->srcs_vec();
  for (uint32_t j_new = 1; j_new <= new_param_use_order.size(); j_new++) {
    auto i_old = old_field_id_to_param_id[new_param_use_order[j_new - 1]];
    insn->set_src(i_old, old_srcs[j_new]);
  }
}
} // namespace

namespace method_dedup {

uint32_t dedup_constructors(const std::vector<DexClass*>& classes,
                            const std::vector<DexClass*>& scope) {
  Timer timer("dedup_constructors");
  std::unordered_map<DexMethod*, DexMethod*> old_to_new;
  CtorSummaries methods_summaries;
  std::unordered_set<DexMethod*> ctor_set;
  std::unordered_map<DexMethodRef*, DexProto*> global_pending_ctor_changes;
  walk::classes(classes, [&](DexClass* cls) {
    auto ctors = cls->get_ctors();
    // Calculate the summaries and group them by super constructor reference.
    std::map<DexMethodRef*, CtorSummaries, dexmethods_comparator>
        grouped_methods;
    for (auto method : ctors) {
      auto summary = summarize_constructor_logic(cls->get_ifields(), method);
      if (!summary) {
        continue;
      }
      grouped_methods[summary->super_ctor][method] = summary;
    }
    // We might need to change the constructor signatures after we finish the
    // deduplication, so we keep a record to avoid collision.
    std::unordered_set<DexProto*> pending_new_protos;
    for (auto& pair : grouped_methods) {
      CtorSummaries& methods = pair.second;
      if (methods.size() < 2) {
        continue;
      }
      // The methods in this group are logically the same, we can use one to
      // represent others with proper transformation.
      auto representative =
          get_representative(methods, cls->get_ifields(), pair.first,
                             &pending_new_protos, &global_pending_ctor_changes);
      if (!representative) {
        TRACE(METH_DEDUP,
              2,
              "%d constructors in %s are the same but not deduplicated.",
              methods.size(),
              SHOW(cls->get_type()));
        continue;
      }
      methods_summaries.insert(methods.begin(), methods.end());
      for (auto& method_summary : methods) {
        auto old_ctor = method_summary.first;
        if (old_ctor != representative) {
          old_to_new[old_ctor] = representative;
          ctor_set.insert(old_ctor);
        }
      }
    }
  });
  // Change callsites.
  auto call_sites = method_reference::collect_call_refs(scope, ctor_set);
  for (auto& callsite : call_sites) {
    auto old_callee = callsite.callee;
    auto& old_param_use_order = methods_summaries[old_callee]->param_use_order;
    auto new_callee = old_to_new[old_callee];
    auto new_param_use_order = methods_summaries[new_callee]->param_use_order;
    auto insn = callsite.mie->insn;
    insn->set_method(new_callee);
    reorder_callsite_args(old_param_use_order, new_param_use_order, insn);
  }
  // Change the constructor representatives to new proto if they need be.
  for (auto& pair : global_pending_ctor_changes) {
    auto method = pair.first;
    DexMethodSpec spec;
    spec.proto = pair.second;
    method->change(spec, /* rename_on_collision */ false,
                   /* update_deobfuscated_name*/ true);
  }
  return old_to_new.size();
}
} // namespace method_dedup
