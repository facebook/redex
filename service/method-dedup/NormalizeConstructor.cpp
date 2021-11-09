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
#include "ReachingDefinitions.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
/**
 * Summary example of a simple constructor whose parameters are only used to
 * initialize a fraction of its instance fields or only being passed to the
 * super constructor. Some or all instsance fields are initialised from args,
 * however every argument must initialise either a field or be passed to
 * the super constructor.
 *
 * void <init>(E e, B b, A a, D d, C c) {
 *   this.f1 = a;
 *   // f2 is not initialised by this constructor
 *   this.f3 = c;
 *   this.f4 = e;
 *   super.<init>(this, b, d);
 * }
 *
 * If the fields are in order of f1, f2, f3, f4 and we assume the super
 * constructor arguments are f5 and f6.
 *
 * The summary of the constructor is
 *   super_ctor : super.<init>
 *   field_id_to_origin is
 *      f1  <-  3
 *      f2  <-  NO_ORIGIN
 *      f3  <-  5
 *      f4  <-  1
 *      super_ctor arg1 <- 2, arg2 <- 4
 *
 * Any two bijective constructors like this in the same class are isomorphic.
 * We cluster constructors together based on their summaries' hashes. Then, we
 * only deduplicate if their summaries are exactly the same.
 *
 * While the logic does not compare instructions one-by-one directly, there are
 * several restrictions regarding a constructor's shape that ensures that
 * summary(method1) == summary(method2) if and only if method1 can be replaced
 * with method2 (and vice-versa).
 *
 * The list of restrictions is:
 *   - the constructors to be deduped can only have load-param, iput, move and
 *     return-void instructions, plus an invoke-direct to the super constructor
 *   - the iput instructions and the invoke-direct will only receive their
 *     source registers form the load-param instructions, and they are not
 *     allowed to share them, e.g. f_i and f_j cannot both be set to parameter
 *     param_k
 */

enum class FieldOriginType { ARG, NO_ORIGIN };

struct FieldOrigin {
  FieldOrigin() : type(FieldOriginType::NO_ORIGIN) {}
  explicit FieldOrigin(uint32_t arg_id)
      : type(FieldOriginType::ARG), arg_id(arg_id) {}

  FieldOriginType type;
  uint32_t arg_id;
};

struct ConstructorSummary {
  DexMethodRef* super_ctor{nullptr};

  // position i represents instance field number i
  std::vector<FieldOrigin> field_id_to_origin;

  ConstructorSummary() = default;

  // used to cluster similar constructors. Note that this is not
  // a sufficient condition for deduplication, the logic relies on
  // the == operator to make that decision.
  size_t hash() const {
    size_t hash = 0;
    for (auto& field_origin : field_id_to_origin) {
      boost::hash_combine(hash, field_origin.type);
      if (field_origin.type == FieldOriginType::NO_ORIGIN) {
        boost::hash_combine(hash, 11);
        boost::hash_combine(hash, "no origin");
      } else {
        boost::hash_combine(hash, 37);
        boost::hash_combine(hash, "argument id");
      }
    }

    boost::hash_combine(hash, field_id_to_origin.size());
    boost::hash_combine(hash, get_arg_ids_origin_size());
    boost::hash_combine(hash, show(super_ctor));

    return hash;
  }

  bool operator==(const ConstructorSummary& other) const {
    if (get_arg_ids_origin_size() != other.get_arg_ids_origin_size()) {
      // same number of fields coming from parameters
      return false;
    }
    if (field_id_to_origin.size() != other.field_id_to_origin.size()) {
      // same number of instance fields
      return false;
    }
    for (size_t i = 0; i < field_id_to_origin.size(); i++) {
      // initialised fields originating from the same argument index
      if (field_id_to_origin[i].type != other.field_id_to_origin[i].type) {
        return false;
      }
    }
    return super_ctor == other.super_ctor;
  }

  size_t get_arg_ids_origin_size() const {
    std::unordered_set<uint32_t> used_arg_ids;
    for (auto origin : field_id_to_origin) {
      if (origin.type == FieldOriginType::ARG) {
        // all fields should be originating from unique parameters
        always_assert(used_arg_ids.count(origin.arg_id) == 0);
        used_arg_ids.insert(origin.arg_id);
      }
    }
    return used_arg_ids.size();
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
    std::vector<FieldOrigin>& ctor_params_to_origin) {
  for (size_t src_idx = 0; src_idx < insn->srcs_size(); src_idx++) {
    auto src = insn->src(src_idx);
    const auto& defs = env.get(src);
    always_assert(!defs.is_bottom() && !defs.is_top());

    // only look for instructions that have a single definition,
    // coming from an object parameter
    if (defs.size() != 1) {
      return false;
    }

    auto* def = *defs.elements().begin();
    if (!opcode::is_a_load_param(def->opcode())) {
      return false;
    }

    auto arg_idx = load_params.at(def);
    if (src_idx == 0) {
      if (arg_idx != 0) {
        return false;
      }
      continue;
    }

    if (used_args.count(arg_idx)) {
      // Do not handle the case in which multiple iputs have their
      // values coming from the same parameters. This makes it simpler
      // to check whether two candidate constructors are actually
      // dedupable.
      return false;
    }
    used_args.insert(arg_idx);
    ctor_params_to_origin.push_back(FieldOrigin(arg_idx));
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
  std::unordered_map<DexFieldRef*, FieldOrigin> field_to_origin;
  std::vector<FieldOrigin> ctor_params_to_origin;
  std::unordered_set<uint32_t> used_args;
  ConstructorSummary summary;

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
        if (summary.super_ctor != nullptr || !method::is_init(ref) ||
            ref->get_class() == method->get_class()) {
          return boost::none;
        }

        summary.super_ctor = ref;
        if (!is_simple_super_invoke(insn, env, load_params, used_args,
                                    ctor_params_to_origin)) {
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
        if (used_args.count(arg_idx)) {
          // Do not handle the case in which multiple iputs have their
          // values coming from the same parameters. This makes it simpler
          // to check whether two candidate constructors are actually
          // dedupable.
          return boost::none;
        }
        used_args.insert(arg_idx);
        field_to_origin.insert({f, FieldOrigin(arg_idx)});
      } else if (opcode::is_a_load_param(opcode) || opcode::is_a_move(opcode) ||
                 opcode::is_return_void(opcode)) {
        // these instructions are allowed inside the constructor
      } else {
        return boost::none;
      }
      reaching_definitions.analyze_instruction(insn, &env);
    }
  }

  if (summary.super_ctor == nullptr) {
    return boost::none;
  }
  for (auto field : ifields) {
    if (field_to_origin.count(field) == 0) {
      summary.field_id_to_origin.push_back(FieldOrigin());
      continue;
    }
    auto& f_orig = field_to_origin[field];
    summary.field_id_to_origin.push_back(f_orig);
  }

  for (auto& f_orig : ctor_params_to_origin) {
    summary.field_id_to_origin.push_back(f_orig);
  }

  // Ensure bijection.
  if (used_args.size() != summary.get_arg_ids_origin_size()) {
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
  for (size_t field_id = 0; field_id < summary.field_id_to_origin.size();
       field_id++) {
    auto origin = summary.field_id_to_origin[field_id];
    if (origin.type == FieldOriginType::ARG) {
      always_assert(origin.arg_id > 0);
      *(new_type_list.begin() + (origin.arg_id - 1)) =
          normalized_typelist[field_id];
    }
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
void reorder_callsite_args(
    const std::vector<FieldOrigin>& old_field_id_to_arg_id,
    const std::vector<FieldOrigin>& new_field_id_to_arg_id,
    IRInstruction* insn) {
  redex_assert(old_field_id_to_arg_id.size() == new_field_id_to_arg_id.size());
  auto old_srcs = insn->srcs_vec();
  for (uint32_t field_id = 0; field_id < new_field_id_to_arg_id.size();
       field_id++) {
    if (new_field_id_to_arg_id[field_id].type != FieldOriginType::ARG) {
      always_assert(old_field_id_to_arg_id[field_id].type !=
                    FieldOriginType::ARG);
      continue;
    }
    always_assert(old_field_id_to_arg_id[field_id].type ==
                  FieldOriginType::ARG);
    uint32_t arg_id = old_field_id_to_arg_id[field_id].arg_id;
    always_assert(arg_id < old_srcs.size());
    insn->set_src(new_field_id_to_arg_id[field_id].arg_id, old_srcs[arg_id]);
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
    std::map<DexMethodRef*, std::unordered_map<size_t, CtorSummaries>,
             dexmethods_comparator>
        grouped_methods;
    for (auto method : ctors) {
      auto summary = summarize_constructor_logic(cls->get_ifields(), method);
      if (!summary) {
        TRACE(METH_DEDUP, 2, "no summary %s\n%s", SHOW(method),
              SHOW(method->get_code()));
        continue;
      }

      grouped_methods[summary->super_ctor][summary->hash()][method] = summary;
    }
    // We might need to change the constructor signatures after we finish the
    // deduplication, so we keep a record to avoid collision.
    std::unordered_set<DexProto*> pending_new_protos;
    for (auto& pair : grouped_methods) {
      for (auto&& [_, cluster] : pair.second) {
        if (cluster.size() < 2) {
          continue;
        }
        // The methods in this group are logically the same, we can use one to
        // represent others with proper transformation.
        auto representative = get_representative(
            cluster, cls->get_ifields(), pair.first, &pending_new_protos,
            &global_pending_ctor_changes);
        if (!representative) {
          TRACE(METH_DEDUP,
                2,
                "%zu constructors in %s are the same but not deduplicated.",
                cluster.size(),
                SHOW(cls->get_type()));
          continue;
        }
        methods_summaries.insert(cluster.begin(), cluster.end());

        auto& representative_summary = cluster[representative];

        for (auto& method_summary : cluster) {
          auto* old_ctor = method_summary.first;
          if (old_ctor != representative &&
              method_summary.second == representative_summary) {
            // if the summaries are the same, old_ctor can be replaced
            // by the representative
            old_to_new[old_ctor] = representative;
            ctor_set.insert(old_ctor);
          }
        }
      }
    }
  });
  // Change callsites.
  auto call_sites = method_reference::collect_call_refs(scope, ctor_set);
  for (auto& callsite : call_sites) {
    auto old_callee = callsite.callee;
    auto& old_field_id_to_arg_id =
        methods_summaries[old_callee]->field_id_to_origin;
    auto new_callee = old_to_new[old_callee];
    redex_assert(new_callee != old_callee);
    auto new_field_id_to_arg_id =
        methods_summaries[new_callee]->field_id_to_origin;
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
