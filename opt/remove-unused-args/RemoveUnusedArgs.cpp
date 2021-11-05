/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnusedArgs.h"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "AnnoUtils.h"
#include "CallGraph.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Liveness.h"
#include "Match.h"
#include "OptData.h"
#include "OptDataDefs.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"

using namespace opt_metadata;

/**
 * The RemoveUnusedArgsPass finds method arguments that are not live in the
 * method body, removes those unused arguments from the method signature, and
 * removes the corresponding argument registers from invocations of that
 * method.
 * As an extra bonus, it also removes unused result types, and it reorderes
 * argument types to reduce the number of needed protos and shorty strings.
 */
namespace remove_unused_args {

constexpr const char* METRIC_CALLSITE_ARGS_REMOVED = "callsite_args_removed";
constexpr const char* METRIC_METHOD_PARAMS_REMOVED = "method_params_removed";
constexpr const char* METRIC_METHODS_UPDATED = "method_signatures_updated";
constexpr const char* METRIC_METHOD_RESULTS_REMOVED = "method_results_removed";
constexpr const char* METRIC_METHOD_PROTOS_REORDERED =
    "method_protos_reordered";
constexpr const char* METRIC_DEAD_INSTRUCTION_COUNT =
    "num_local_dce_dead_instruction_count";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTION_COUNT =
    "num_local_dce_unreachable_instruction_count";
constexpr const char* METRIC_ITERATIONS = "iterations";

/**
 * Returns metrics as listed above from running RemoveArgs:
 * run() removes unused params from method signatures and param loads, then
 * updates all affected callsites accordingly.
 */
RemoveArgs::PassStats RemoveArgs::run(ConfigFiles& config) {
  RemoveArgs::PassStats pass_stats;
  gather_results_used();
  auto override_graph = mog::build_graph(m_scope);
  compute_reordered_protos(*override_graph);
  auto method_stats =
      update_method_protos(*override_graph, config.get_do_not_devirt_anon());
  pass_stats.method_params_removed_count =
      method_stats.method_params_removed_count;
  pass_stats.methods_updated_count = method_stats.methods_updated_count;
  pass_stats.callsite_args_removed_count = update_callsites();
  pass_stats.method_results_removed_count =
      method_stats.method_results_removed_count;
  pass_stats.method_protos_reordered_count =
      method_stats.method_protos_reordered_count;
  pass_stats.local_dce_stats = method_stats.local_dce_stats;
  return pass_stats;
}

/**
 * Inspects all invoke instructions, and whether they are followed by
 * move-result instructions, and record this information for each method.
 */
void RemoveArgs::gather_results_used() {
  walk::parallel::code(
      m_scope, [&result_used = m_result_used](DexMethod*, IRCode& code) {
        const auto ii = InstructionIterable(code);
        for (auto it = ii.begin(); it != ii.end(); it++) {
          auto insn = it->insn;
          if (!opcode::is_an_invoke(insn->opcode())) {
            continue;
          }
          const auto next = std::next(it);
          always_assert(next != ii.end());
          const auto peek = next->insn;
          if (!opcode::is_a_move_result(peek->opcode())) {
            continue;
          }
          auto method = insn->get_method()->as_def();
          if (!method) {
            // TODO: T31388603 -- Remove unused results for true virtuals.
            continue;
          }
          result_used.insert(method);
        }
      });
}

// For normalization, we put primitive types last and thus all reference types
// first, as shorty strings take up space in a dex but don't distinguish arrays
// and classes.
static bool compare_dextypes_for_normalization(const DexType* a,
                                               const DexType* b) {
  int a_primitive = type::is_primitive(a) ? 1 : 0;
  int b_primitive = type::is_primitive(b) ? 1 : 0;
  if (a_primitive != b_primitive) {
    return a_primitive < b_primitive;
  }
  return compare_dexstrings(a->get_name(), b->get_name());
}

static DexProto* normalize_proto(DexProto* proto) {
  DexTypeList::ContainerType args_copy(proto->get_args()->begin(),
                                       proto->get_args()->end());
  std::stable_sort(args_copy.begin(), args_copy.end(),
                   compare_dextypes_for_normalization);
  DexType* rtype = proto->get_rtype();
  return DexProto::make_proto(
      rtype, DexTypeList::make_type_list(std::move(args_copy)));
}

static bool compare_weighted_dexprotos(const std::pair<DexProto*, size_t>& a,
                                       const std::pair<DexProto*, size_t>& b) {
  if (a.second != b.second) {
    return a.second > b.second;
  }
  return compare_dexprotos(a.first, b.first);
}

static bool any_external(const std::unordered_set<const DexMethod*>& methods) {
  for (auto method : methods) {
    auto cls = type_class(method->get_class());
    if (cls == nullptr || cls->is_external()) {
      return true;
    }
  }
  return false;
}

/**
 * Inspects all methods and invoke instructions, building up a set of protos
 * that we should change. This is done by identifying all defined protos, and
 * then removing those from consideration that we should not change, e.g. those
 * that are externally defined or not-renamable.
 */
void RemoveArgs::compute_reordered_protos(const mog::Graph& override_graph) {
  ConcurrentMap<DexProto*, size_t> fixed_protos;
  ConcurrentSet<DexProto*> defined_protos;
  auto record_fixed_proto = [&fixed_protos](DexProto* proto, size_t increment) {
    fixed_protos.update(proto,
                        [increment](DexProto*, size_t& count,
                                    bool /* exists */) { count += increment; });
  };
  walk::parallel::methods(
      m_scope,
      [&override_graph, &record_fixed_proto,
       &defined_protos](DexMethod* caller) {
        auto caller_proto = caller->get_proto();
        defined_protos.insert(caller_proto);
        if (!can_rename(caller) || is_native(caller)) {
          record_fixed_proto(caller_proto, 1);
        } else if (caller->is_virtual()) {
          auto is_interface_method =
              is_interface(type_class(caller->get_class()));
          if ((is_interface_method && (root(caller) || !can_rename(caller)))) {
            // We cannot rule out that there are dynamically added classes,
            // created via Proxy.newProxyInstance, that override this method. So
            // we assume the worst.
            record_fixed_proto(caller_proto, 0);
          } else {
            auto& node = override_graph.get_node(caller);
            if (any_external(node.parents)) {
              // We can't change the signature of an overriding method when the
              // overridden method is external
              record_fixed_proto(caller_proto, 0);
            } else if (is_interface_method && any_external(node.children)) {
              // This captures the case where an interface defines a method
              // whose only implementation is one that is inherited from an
              // external base class.
              record_fixed_proto(caller_proto, 0);
            }
          }
        }

        IRCode* code = caller->get_code();
        if (code == nullptr) {
          return;
        }
        for (const auto& mie : InstructionIterable(code)) {
          if (mie.insn->has_method()) {
            auto callee = mie.insn->get_method();
            auto callee_proto = callee->get_proto();
            // We don't resolve here, but just check if the provided callee is
            // already resolved. If not, we are going to be conservative. (Note
            // that this matches what update_callsite does below.) We are also
            // going to record any external callees as fixed.
            DexMethod* callee_def = callee->as_def();
            if (callee_def == nullptr || callee_def->is_external()) {
              record_fixed_proto(callee_proto, 1);
            }
          }
        }
      });

  std::vector<std::pair<DexProto*, size_t>> ordered_fixed_protos(
      fixed_protos.begin(), fixed_protos.end());
  std::sort(ordered_fixed_protos.begin(), ordered_fixed_protos.end(),
            compare_weighted_dexprotos);
  std::unordered_map<DexProto*, DexProto*> fixed_representatives;
  for (auto p : ordered_fixed_protos) {
    auto proto = p.first;
    auto normalized_proto = normalize_proto(proto);
    // First one (with most references) wins
    fixed_representatives.emplace(normalized_proto, proto);
  }
  for (auto proto : defined_protos) {
    if (fixed_protos.count(proto)) {
      continue;
    }
    auto reordered_proto = normalize_proto(proto);
    auto it = fixed_representatives.find(reordered_proto);
    if (it != fixed_representatives.end()) {
      reordered_proto = it->second;
    }
    if (proto != reordered_proto) {
      m_reordered_protos.emplace(proto, reordered_proto);
    }
  }

  TRACE(ARGS, 1, "[compute_reordered_protos] can reorder %zu method protos",
        m_reordered_protos.size());
}

/**
 * Returns a vector of live argument indices.
 * Updates dead_insns with the load_params that await removal.
 * For instance methods, the 'this' argument is always considered live.
 * e.g. We return {0, 2} for a method whose 0th and 2nd args are live.
 *
 * NOTE: In the IR, invoke instructions specify exactly one register
 *       for any param size.
 */
std::deque<uint16_t> compute_live_args(
    DexMethod* method,
    size_t num_args,
    std::vector<IRInstruction*>* dead_insns) {
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());
  auto entry_block = cfg.entry_block();

  std::deque<uint16_t> live_arg_idxs;
  bool is_instance_method = !is_static(method);
  size_t last_arg_idx = is_instance_method ? num_args : num_args - 1;
  auto first_insn = entry_block->get_first_insn()->insn;
  // live_vars contains all the registers needed by entry_block's
  // successors.
  auto live_vars = fixpoint_iter.get_live_out_vars_at(entry_block);

  for (auto it = entry_block->rbegin(); it != entry_block->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = it->insn;
    if (opcode::is_a_load_param(insn->opcode())) {
      if (live_vars.contains(insn->dest()) ||
          (is_instance_method && it->insn == first_insn)) {
        // Mark live args live, and always mark the "this" arg live.
        live_arg_idxs.push_front(last_arg_idx);
      } else {
        dead_insns->emplace_back(it->insn);
      }
      last_arg_idx--;
    }
    fixpoint_iter.analyze_instruction(insn, &live_vars);
  }

  always_assert(live_arg_idxs.size() + dead_insns->size() == is_instance_method
                    ? num_args + 1
                    : num_args);

  return live_arg_idxs;
}

/**
 * Returns an updated argument type list for the given method with the given
 * live argument indices.
 */
DexTypeList::ContainerType RemoveArgs::get_live_arg_type_list(
    DexMethod* method, const std::deque<uint16_t>& live_arg_idxs) {
  DexTypeList::ContainerType live_args;
  auto args_list = method->get_proto()->get_args();

  for (uint16_t arg_num : live_arg_idxs) {
    if (!is_static(method)) {
      if (arg_num == 0) {
        continue;
      }
      arg_num--;
    }
    live_args.push_back(args_list->at(arg_num));
  }
  return live_args;
}

/**
 * Returns true on successful update to the given method's signature, where
 * the updated args list is specified by live_args.
 */
bool RemoveArgs::update_method_signature(
    DexMethod* method,
    const std::deque<uint16_t>& live_arg_idxs,
    bool remove_result,
    DexProto* reordered_proto) {
  always_assert_log(method->is_def(),
                    "We don't treat virtuals, so methods must be defined\n");

  const auto& full_name = method->get_deobfuscated_name_or_empty();
  for (const auto& s : m_blocklist) {
    if (full_name.find(s) != std::string::npos) {
      TRACE(ARGS, 3,
            "Skipping {%s} due to black list match of {%s} against {%s}",
            SHOW(method), full_name.c_str(), s.c_str());
      return false;
    }
  }

  DexProto* updated_proto;
  DexTypeList* live_args_list{nullptr};
  if (reordered_proto != nullptr) {
    updated_proto = reordered_proto;
  } else {
    auto live_args = get_live_arg_type_list(method, live_arg_idxs);
    live_args_list = DexTypeList::make_type_list(std::move(live_args));
    DexType* rtype =
        remove_result ? type::_void() : method->get_proto()->get_rtype();
    updated_proto = DexProto::make_proto(rtype, live_args_list);
  }
  always_assert(updated_proto != method->get_proto());

  auto colliding_mref = DexMethod::get_method(
      method->get_class(), method->get_name(), updated_proto);
  if (colliding_mref) {
    auto colliding_method = colliding_mref->as_def();
    if (colliding_method && method::is_constructor(colliding_method)) {
      // We can't rename constructors, so we give up on removing args.
      return false;
    }
  }

  auto name = method->get_name();
  if (method->is_virtual()) {
    // TODO: T31388603 -- Remove unused args for true virtuals.

    // When changing the proto, we need to worry about changes to virtual scopes
    // --- for this particular method change, but also across all other upcoming
    // method changes. To this end, we introduce unique names for each name and
    // arg list to avoid any such overlaps.

    NamedRenameMap& named_rename_map = m_rename_maps[name];
    size_t name_index;
    std::string kind;
    if (reordered_proto) {
      // When we reorder protos, possibly for entire virtual scopes, we need to
      // make the name unique for each virtual scope, which is defined by the
      // pair (name, original proto args).

      // "rvp" stands for reordered virtual proto
      kind = "$rvp";
      auto original_args = method->get_proto()->get_args();
      auto it = named_rename_map.reordering_uniquifiers.find(original_args);
      if (it != named_rename_map.reordering_uniquifiers.end()) {
        name_index = it->second;
      } else {
        named_rename_map.reordering_uniquifiers.emplace(
            original_args,
            name_index = named_rename_map.next_reordering_uniquifiers++);
      }
    } else {
      // We currently only change protos for non-true virtual methods, we need
      // to worry about creating collisions across other proto changes. Thus, we
      // make the name unique for each instance of the pair (name, final proto
      // args).

      // "uva" stands for unused virtual args
      kind = "$uva";
      name_index = named_rename_map.general_uniquifiers[live_args_list]++;
    }

    std::stringstream ss;
    // This pass typically runs before the obfuscation pass, so we should not
    // need to be too concerned here about creating long method names.

    ss << name->str() << kind << std::to_string(m_iteration) << "$"
       << std::to_string(name_index);
    name = DexString::make_string(ss.str());
  }

  DexMethodSpec spec(nullptr, name, updated_proto);
  method->change(spec, true /* rename on collision */);

  // We make virtual method names unique via $rvp / $uva name mangling; check
  // that this worked:
  always_assert(!method->is_virtual() || method->get_name() == name);

  TRACE(ARGS, 3, "Method signature updated to %s", SHOW(method));
  log_opt(METHOD_PARAMS_REMOVED, method);
  return true;
}

static void compute_dead_insns_and_remove_result(
    DexMethod* method,
    const mog::Graph& override_graph,
    const ConcurrentSet<DexMethod*>& results_used,
    std::deque<uint16_t>* live_arg_idxs,
    std::vector<IRInstruction*>* dead_insns,
    bool* remove_result) {
  if (method->get_code() == nullptr) {
    return;
  }

  if (!can_rename(method)) {
    // Nothing to do if ProGuard says we can't change the method args.
    TRACE(ARGS,
          5,
          "Method is disqualified from being updated by ProGuard rules: %s",
          SHOW(method));
    return;
  }

  // If a method is devirtualizable, proceed with live arg computation.
  if (method->is_virtual() && mog::is_true_virtual(override_graph, method)) {
    // TODO: T31388603 -- Remove unused args for true virtuals.
    return;
  }

  auto proto = method->get_proto();
  auto result_used = !!results_used.count_unsafe(method);
  auto num_args = proto->get_args()->size();
  *remove_result = !proto->is_void() && !result_used;

  // For instance methods, num_args does not count the 'this' argument.
  if (num_args == 0 && !remove_result) {
    // Nothing to do if the method doesn't have args or result to remove.
    return;
  }

  *live_arg_idxs = compute_live_args(method, num_args, dead_insns);
}

// When reordering a method's proto, we need to update the method's load-param
// instructions accordingly. We return the accordingly reshuffled list of (live)
// argument indices.
static std::deque<uint16_t> update_method_body_for_reordered_proto(
    DexMethod* method, DexProto* original_proto, DexProto* reordered_proto) {
  // We store a copy of opcode, reg to enable re-assigning in permutated
  // order
  struct LoadParamInfo {
    IROpcode opcode;
    reg_t reg;
    IRInstruction* insn;
  };
  std::vector<LoadParamInfo> load_param_infos;
  boost::optional<std::vector<LoadParamInfo>::iterator> load_param_infos_it;
  if (method->get_code()) {
    auto param_insns = method->get_code()->get_param_instructions();
    for (const auto& mie : InstructionIterable(param_insns)) {
      load_param_infos.push_back(
          {mie.insn->opcode(), mie.insn->dest(), mie.insn});
    }
    load_param_infos_it = load_param_infos.begin();
  }

  std::deque<uint16_t> idxs;
  std::unordered_map<DexType*, std::deque<uint16_t>> idxs_by_type;
  int idx = 0;
  if (!is_static(method)) {
    idxs.push_back(idx++);
    if (load_param_infos_it) {
      (*load_param_infos_it)++;
    }
  }
  for (auto t : *original_proto->get_args()) {
    idxs_by_type[t].push_back(idx++);
  }

  for (auto t : *reordered_proto->get_args()) {
    auto& deque = idxs_by_type.find(t)->second;
    auto new_idx = deque.front();
    deque.pop_front();

    idxs.push_back(new_idx);

    if (load_param_infos_it) {
      auto load_param_insn = (*load_param_infos_it)++->insn;
      auto& info = load_param_infos.at(new_idx);
      load_param_insn->set_opcode(info.opcode);
      load_param_insn->set_dest(info.reg);
    }
  }

  return idxs;
}

/**
 * For methods that have unused arguments, record live argument registers.
 */
RemoveArgs::MethodStats RemoveArgs::update_method_protos(
    const mog::Graph& override_graph,
    const std::unordered_set<DexType*>& no_devirtualize_annos) {
  // Phase 1: Find (in parallel) all methods that we can potentially update

  struct Entry {
    std::vector<IRInstruction*> dead_insns;
    std::deque<uint16_t> live_arg_idxs;
    bool remove_result;
    DexProto* original_proto;
    DexProto* reordered_proto;
  };
  ConcurrentMap<DexMethod*, Entry> unordered_entries;
  walk::parallel::methods(m_scope, [&](DexMethod* method) {
    std::deque<uint16_t> live_arg_idxs;
    std::vector<IRInstruction*> dead_insns;
    bool remove_result{false};
    DexProto* reordered_proto{nullptr};

    auto it = m_reordered_protos.find(method->get_proto());
    if (it != m_reordered_protos.end()) {
      reordered_proto = it->second;
    } else {
      // Only if there's no reordering, we'll look at dead args and results
      compute_dead_insns_and_remove_result(method, override_graph,
                                           m_result_used, &live_arg_idxs,
                                           &dead_insns, &remove_result);
      if (dead_insns.empty() && !remove_result) {
        return;
      }
    }
    // If method is annotated with @DoNotVirtualize, careful about removing this
    // param that might make the method static. For now do not remove any unused
    // arguments for this.
    if (has_any_annotation(method, no_devirtualize_annos)) {
      return;
    }

    // Remember entry
    unordered_entries.emplace(method,
                              (Entry){dead_insns, live_arg_idxs, remove_result,
                                      method->get_proto(), reordered_proto});
  });

  // Phase 2: Deterministically update proto (including (re)name as needed)

  // Sort entries, so that we process all renaming operations in a
  // deterministic order.
  std::vector<std::pair<DexMethod*, Entry>> ordered_entries(
      unordered_entries.begin(), unordered_entries.end());
  std::sort(ordered_entries.begin(), ordered_entries.end(),
            [](const std::pair<DexMethod*, Entry>& a,
               const std::pair<DexMethod*, Entry>& b) {
              return compare_dexmethods(a.first, b.first);
            });

  RemoveArgs::MethodStats method_stats;
  std::vector<DexClass*> classes;
  std::unordered_map<DexClass*, std::vector<std::pair<DexMethod*, Entry>>>
      class_entries;
  for (auto& p : ordered_entries) {
    DexMethod* method = p.first;
    const Entry& entry = p.second;
    if (!update_method_signature(method, entry.live_arg_idxs,
                                 entry.remove_result, entry.reordered_proto)) {
      continue;
    }

    // Remember entry for further processing, and log statistics
    DexClass* cls = type_class(method->get_class());
    classes.push_back(cls);
    class_entries[cls].push_back(p);
    method_stats.methods_updated_count++;
    method_stats.method_params_removed_count += entry.dead_insns.size();
    method_stats.method_results_removed_count += entry.remove_result ? 1 : 0;
    method_stats.method_protos_reordered_count += entry.reordered_proto ? 1 : 0;
  }
  sort_unique(classes);

  // Phase 3: Update body of updated methods (in parallel)

  std::mutex local_dce_stats_mutex;
  auto& local_dce_stats = method_stats.local_dce_stats;
  walk::parallel::classes(classes, [&](DexClass* cls) {
    for (auto& p : class_entries.at(cls)) {
      DexMethod* method = p.first;
      const Entry& entry = p.second;

      if (!entry.dead_insns.empty()) {
        // We update the method signature, so we must remove unused
        // OPCODE_LOAD_PARAM_* to satisfy IRTypeChecker.
        for (auto dead_insn : entry.dead_insns) {
          method->get_code()->remove_opcode(dead_insn);
        }
        m_live_arg_idxs_map.emplace(method, entry.live_arg_idxs);
      }

      if (entry.remove_result) {
        for (const auto& mie : InstructionIterable(method->get_code())) {
          auto insn = mie.insn;
          if (opcode::is_a_return_value(insn->opcode())) {
            insn->set_opcode(OPCODE_RETURN_VOID);
            insn->set_srcs_size(0);
          }
        }

        std::unordered_set<DexMethodRef*> pure_methods;
        auto local_dce =
            LocalDce(&m_init_classes_with_side_effects, pure_methods);
        local_dce.dce(method->get_code(), /* normalize_new_instances */ true,
                      method->get_class());
        const auto& stats = local_dce.get_stats();
        if (stats.dead_instruction_count |
            stats.unreachable_instruction_count) {
          std::lock_guard<std::mutex> lock(local_dce_stats_mutex);
          local_dce_stats += stats;
        }
      }

      if (entry.reordered_proto != nullptr) {
        auto idxs = update_method_body_for_reordered_proto(
            method, entry.original_proto, entry.reordered_proto);
        m_live_arg_idxs_map.emplace(method, std::move(idxs));
      }
    }
  });
  return method_stats;
}

/**
 * Removes dead arguments from the given invoke instr if applicable.
 * Returns the number of arguments removed.
 */
size_t RemoveArgs::update_callsite(IRInstruction* instr) {
  auto method = instr->get_method()->as_def();
  if (!method) {
    // TODO: T31388603 -- Remove unused args for true virtuals.
    return 0;
  };

  auto kv_pair = m_live_arg_idxs_map.find(method);
  if (kv_pair == m_live_arg_idxs_map.end()) {
    // No removable arguments, so do nothing.
    return 0;
  }
  auto& updated_srcs = kv_pair->second;
  std::vector<reg_t> new_srcs;
  for (size_t i = 0; i < updated_srcs.size(); ++i) {
    new_srcs.push_back(instr->src(updated_srcs.at(i)));
  }
  for (size_t i = 0; i < new_srcs.size(); ++i) {
    instr->set_src(i, new_srcs.at(i));
  }
  always_assert_log(instr->srcs_size() >= updated_srcs.size(),
                    "In RemoveArgs, callsites always update to fewer args, or "
                    "same in case of reordering\n");
  auto callsite_args_removed = instr->srcs_size() - updated_srcs.size();
  instr->set_srcs_size(updated_srcs.size());
  return callsite_args_removed;
}

/**
 * Removes unused arguments at callsites and returns the number of arguments
 * removed.
 */
size_t RemoveArgs::update_callsites() {
  // Walk through all methods to look for and edit callsites.
  return walk::parallel::methods<size_t>(
      m_scope, [&](DexMethod* method) -> size_t {
        auto code = method->get_code();
        if (code == nullptr) {
          return 0;
        }
        size_t callsite_args_removed = 0;
        for (const auto& mie : InstructionIterable(code)) {
          auto insn = mie.insn;
          if (opcode::is_an_invoke(insn->opcode())) {
            size_t insn_args_removed = update_callsite(insn);
            if (insn_args_removed > 0) {
              log_opt(CALLSITE_ARGS_REMOVED, method, insn);
              callsite_args_removed += insn_args_removed;
            }
          }
        }
        return callsite_args_removed;
      });
}

void RemoveUnusedArgsPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());

  size_t num_callsite_args_removed = 0;
  size_t num_method_params_removed = 0;
  size_t num_methods_updated = 0;
  size_t num_method_results_removed_count = 0;
  size_t num_method_protos_reordered_count = 0;
  size_t num_iterations = 0;
  LocalDce::Stats local_dce_stats;
  while (true) {
    num_iterations++;
    RemoveArgs rm_args(scope, init_classes_with_side_effects, m_blocklist,
                       m_total_iterations++);
    auto pass_stats = rm_args.run(conf);
    if (pass_stats.methods_updated_count == 0) {
      break;
    }
    num_callsite_args_removed += pass_stats.callsite_args_removed_count;
    num_method_params_removed += pass_stats.method_params_removed_count;
    num_methods_updated += pass_stats.methods_updated_count;
    num_method_results_removed_count += pass_stats.method_results_removed_count;
    num_method_protos_reordered_count +=
        pass_stats.method_protos_reordered_count;
    local_dce_stats += pass_stats.local_dce_stats;
  }

  TRACE(ARGS,
        1,
        "Removed %zu redundant callsite arguments",
        num_callsite_args_removed);
  TRACE(ARGS,
        1,
        "Removed %zu redundant method parameters",
        num_method_params_removed);
  TRACE(ARGS,
        1,
        "Removed %zu redundant method results",
        num_method_results_removed_count);
  TRACE(ARGS, 1, "Reordered %zu method protos",
        num_method_protos_reordered_count);
  TRACE(ARGS,
        1,
        "Updated %zu methods with redundant parameters",
        num_methods_updated);

  mgr.set_metric(METRIC_CALLSITE_ARGS_REMOVED, num_callsite_args_removed);
  mgr.set_metric(METRIC_METHOD_PARAMS_REMOVED, num_method_params_removed);
  mgr.set_metric(METRIC_METHODS_UPDATED, num_methods_updated);
  mgr.set_metric(METRIC_METHOD_RESULTS_REMOVED,
                 num_method_results_removed_count);
  mgr.set_metric(METRIC_METHOD_PROTOS_REORDERED,
                 num_method_protos_reordered_count);
  mgr.set_metric(METRIC_DEAD_INSTRUCTION_COUNT,
                 local_dce_stats.dead_instruction_count);
  mgr.set_metric(METRIC_UNREACHABLE_INSTRUCTION_COUNT,
                 local_dce_stats.unreachable_instruction_count);
  mgr.set_metric(METRIC_ITERATIONS, num_iterations);
}

static RemoveUnusedArgsPass s_pass;
} // namespace remove_unused_args
