/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NormalizeConstructor.h"

#include "DexClass.h"
#include "EditableCfgAdapter.h"
#include "IRInstruction.h"
#include "MethodReference.h"
#include "ReachingDefinitions.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
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
 * If the fields are in order of f1, f2, f3, and we assume the super constructor argument is f4.
 *
 * The summary of the constructor is
 *   super_ctor : super.<init>
 *   field_id_to_arg_id is
 *      f1  <-  2
 *      f2  <-  1
 *      f3  <-  4
 *      super_ctor arg1 <- 3
 *
 * Any two bijective constructors like this in the same class are isomorphic.
 */
// clang-format on
struct ConstructorSummary {
  DexMethodRef* super_ctor{nullptr};
  // 1 to N to represent argument id.
  std::vector<uint32_t> field_id_to_arg_id;

  ConstructorSummary(DexMethodRef* ctor, size_t num_fields) : super_ctor(ctor) {
    field_id_to_arg_id.reserve(num_fields +
                               ctor->get_proto()->get_args()->size());
  }
};

/*
 * The invoke-direct instruction to the super constructor should have
 * all src registers coming from a unique argument.
 */
bool is_simple_super_invoke(
    IRInstruction* insn,
    reaching_defs::Environment& env,
    const std::unordered_map<IRInstruction*, uint32_t>& load_params,
    std::unordered_set<uint32_t>& used_args,
    std::vector<uint32_t>& ctor_params_to_arg_id) {
  for (size_t src_idx = 0; src_idx < insn->srcs_size(); src_idx++) {
    auto src = insn->src(src_idx);
    const auto& defs = env.get(src);
    always_assert(!defs.is_bottom() && !defs.is_top());

    // only look for instructions that have a single definition,
    // coming from an object parameter
    if (defs.size() != 1) {
      return false;
    }

    if (src_idx == 0) {
      auto* load_param_this = *defs.elements().begin();
      if (load_params.count(load_param_this) == 0 ||
          load_params.at(load_param_this) != 0) {
        return false;
      }
      continue;
    }
    auto* def = *defs.elements().begin();
    if (!opcode::is_a_load_param(def->opcode())) {
      return false;
    }
    auto arg_idx = load_params.at(def);

    ctor_params_to_arg_id.push_back(arg_idx);
    used_args.insert(arg_idx);
  }
  return true;
}

/**
 * @param ifields : Should include all the instance fields of the class.
 */
boost::optional<ConstructorSummary> summarize_constructor_logic(
    const std::vector<DexField*>& ifields, DexMethod* method) {
  if (root(method) || !is_constructor(method) || !method::is_init(method) ||
      method->get_code() == nullptr) {
    return boost::none;
  }
  IRInstruction* super_ctor_invocation = nullptr;
  std::unordered_map<DexFieldRef*, uint32_t> field_to_arg_id;
  std::vector<uint32_t> ctor_params_to_arg_id;
  std::unordered_set<uint32_t> used_args;

  cfg::ScopedCFG cfg(method->get_code());

  std::unordered_map<IRInstruction*, uint32_t> load_params;
  uint32_t param_idx{0};
  auto param_instructions = cfg->get_param_instructions();
  for (auto& param_insn : param_instructions) {
    auto* insn = param_insn.insn;
    load_params.emplace(insn, param_idx++);
  }

  // TODO: Give up if there's exception handling.
  reaching_defs::MoveAwareFixpointIterator reaching_definitions(*cfg);
  reaching_definitions.run({});
  for (cfg::Block* block : cfg->blocks()) {
    auto env = reaching_definitions.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto* insn = it->insn;
      auto opcode = insn->opcode();
      if (opcode::is_invoke_direct(opcode)) {
        auto ref = insn->get_method();
        if (super_ctor_invocation != nullptr || !method::is_init(ref) ||
            ref->get_class() == method->get_class()) {
          return boost::none;
        }

        super_ctor_invocation = insn;
        if (!is_simple_super_invoke(insn, env, load_params, used_args,
                                    ctor_params_to_arg_id)) {
          return boost::none;
        }
      } else if (opcode::is_an_iput(opcode)) {
        redex_assert(insn->srcs_size() == 2);
        auto* f = insn->get_field();
        auto src = insn->src(0);
        const auto& defs = env.get(src);
        always_assert(!defs.is_bottom() && !defs.is_top());
        if (defs.size() != 1) {
          return boost::none;
        }
        auto* def = *defs.elements().begin();
        if (!opcode::is_a_load_param(def->opcode())) {
          return boost::none;
        }
        auto arg_idx = load_params.at(def);

        field_to_arg_id.insert({f, arg_idx});
        used_args.insert(arg_idx);
      } else if (opcode::is_a_load_param(opcode) || opcode::is_a_move(opcode) ||
                 opcode::is_return_void(opcode)) {
      } else {
        return boost::none;
      }
      reaching_definitions.analyze_instruction(insn, &env);
    }
  }

  if (super_ctor_invocation == nullptr) {
    return boost::none;
  }

  if (field_to_arg_id.size() != ifields.size()) {
    return boost::none;
  }
  ConstructorSummary summary(super_ctor_invocation->get_method(),
                             ifields.size());
  for (auto field : ifields) {
    auto arg_id = field_to_arg_id[field];
    summary.field_id_to_arg_id.push_back(arg_id);
  }

  for (auto& f_orig : ctor_params_to_arg_id) {
    summary.field_id_to_arg_id.push_back(f_orig);
  }

  // Ensure bijection.
  if (used_args.size() != summary.field_id_to_arg_id.size()) {
    return boost::none;
  }
  if (used_args.size() != method->get_proto()->get_args()->size()) {
    // Not support methods with unused arguments. We can remove unused arguments
    // or reordering the instructions first.
    return boost::none;
  }
  return summary;
}

using CtorSummaries = std::
    map<DexMethod*, boost::optional<ConstructorSummary>, dexmethods_comparator>;

/**
 * A constructor representative needs a more "generic" proto.
 * For example, if the first argument is assigned to a field in
 * Ljava/lang/Object; type,
 * void <init>(LTypeA;I) =>
 *    void <init>Ljava/lang/Object;I)
 */
DexProto* generalize_proto(const std::vector<DexType*>& normalized_typelist,
                           const ConstructorSummary& summary,
                           const DexProto* original_proto) {
  DexTypeList::ContainerType new_type_list{original_proto->get_args()->begin(),
                                           original_proto->get_args()->end()};
  for (size_t field_id = 0; field_id < summary.field_id_to_arg_id.size();
       field_id++) {
    auto arg_id = summary.field_id_to_arg_id[field_id];
    *(new_type_list.begin() + (arg_id - 1)) = normalized_typelist[field_id];
  }
  return DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list(std::move(new_type_list)));
}

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
    auto new_proto =
        generalize_proto(normalized_typelist, *summary, method->get_proto());
    if (new_proto == method->get_proto()) {
      return method;
    }
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
 * set_src(new_arg_id, src(old_arg_id)) when the new_arg_id and the old_arg_id
 * are assigning to the same field.
 */
void reorder_callsite_args(const std::vector<uint32_t>& old_field_id_to_arg_id,
                           const std::vector<uint32_t>& new_field_id_to_arg_id,
                           IRInstruction* insn) {
  redex_assert(old_field_id_to_arg_id.size() == new_field_id_to_arg_id.size());
  auto old_srcs = insn->srcs_vec();
  for (uint32_t field_id = 0; field_id < new_field_id_to_arg_id.size();
       field_id++) {
    insn->set_src(new_field_id_to_arg_id[field_id],
                  old_srcs[old_field_id_to_arg_id[field_id]]);
  }
}
} // namespace

namespace method_dedup {

uint32_t estimate_deduplicatable_ctor_code_size(const DexClass* cls) {
  const auto& ifields = cls->get_ifields();
  uint32_t estimated_size = 0;
  for (auto method : cls->get_ctors()) {
    auto summary = summarize_constructor_logic(ifields, method);
    if (!summary) {
      continue;
    }
    estimated_size += method->get_code()->sum_opcode_sizes() +
                      /*estimated encoded_method size*/ 2 +
                      /*method_id_item size*/ 8;
  }
  return estimated_size;
}

uint32_t dedup_constructors(const std::vector<DexClass*>& classes,
                            const std::vector<DexClass*>& scope) {
  Timer timer("dedup_constructors");
  std::unordered_map<DexMethod*, DexMethod*> old_to_new;
  CtorSummaries methods_summaries;
  std::unordered_set<DexMethod*> ctor_set;
  std::unordered_map<DexMethodRef*, DexProto*> global_pending_ctor_changes;
  walk::classes(classes, [&](DexClass* cls) {
    auto ctors = cls->get_ctors();
    if (ctors.size() < 2) {
      return;
    }
    // Calculate the summaries and group them by super constructor reference.
    std::map<DexMethodRef*, CtorSummaries, dexmethods_comparator>
        grouped_methods;
    for (auto method : ctors) {
      auto summary = summarize_constructor_logic(cls->get_ifields(), method);
      if (!summary) {
        TRACE(METH_DEDUP, 2, "no summary %s\n%s", SHOW(method),
              SHOW(method->get_code()));
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
              "%zu constructors in %s are the same but not deduplicated.",
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
    auto& old_field_id_to_arg_id =
        methods_summaries[old_callee]->field_id_to_arg_id;
    auto new_callee = old_to_new[old_callee];
    redex_assert(new_callee != old_callee);
    auto new_field_id_to_arg_id =
        methods_summaries[new_callee]->field_id_to_arg_id;
    auto insn = callsite.mie->insn;
    insn->set_method(new_callee);
    reorder_callsite_args(old_field_id_to_arg_id, new_field_id_to_arg_id, insn);
  }
  // Change the constructor representatives to new proto if they need be.
  for (auto& pair : global_pending_ctor_changes) {
    auto method = pair.first;
    DexMethodSpec spec;
    spec.proto = pair.second;
    method->change(spec, /* rename_on_collision */ false);
  }
  TRACE(METH_DEDUP, 2, "normalized-deduped constructors %zu",
        old_to_new.size());
  return old_to_new.size();
}
} // namespace method_dedup
