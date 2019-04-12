/**
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

#include "CallGraph.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Liveness.h"
#include "Match.h"
#include "OptData.h"
#include "OptDataDefs.h"
#include "TypeSystem.h"
#include "VirtualScope.h"
#include "Walkers.h"

using namespace opt_metadata;

/**
 * The RemoveUnusedArgsPass finds method arguments that are not live in the
 * method body, removes those unused arguments from the method signature, and
 * removes the corresponding argument registers from invocations of that
 * method.
 */
namespace remove_unused_args {

constexpr const char* METRIC_CALLSITE_ARGS_REMOVED = "callsite_args_removed";
constexpr const char* METRIC_METHOD_PARAMS_REMOVED = "method_params_removed";
constexpr const char* METRIC_METHODS_UPDATED = "method_signatures_updated";

/**
 * Returns metrics as listed above from running RemoveArgs:
 * run() removes unused params from method signatures and param loads, then
 * updates all affected callsites accordingly.
 */
RemoveArgs::PassStats RemoveArgs::run() {
  RemoveArgs::PassStats pass_stats;
  auto method_stats = update_meths_with_unused_args();
  pass_stats.method_params_removed_count =
      method_stats.method_params_removed_count;
  pass_stats.methods_updated_count = method_stats.methods_updated_count;
  pass_stats.callsite_args_removed_count = update_callsites();
  return pass_stats;
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
std::deque<uint16_t> RemoveArgs::compute_live_args(
    DexMethod* method,
    size_t num_args,
    std::vector<IRInstruction*>* dead_insns) {
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain(code->get_registers_size()));
  auto entry_block = cfg.entry_block();

  std::deque<uint16_t> live_arg_idxs;
  bool is_instance_method = !is_static(method);
  size_t last_arg_idx = is_instance_method ? num_args : num_args - 1;
  auto first_insn = entry_block->get_first_insn()->insn;
  // live_vars contains all the registers needed by entry_block's successors.
  auto live_vars = fixpoint_iter.get_live_out_vars_at(entry_block);

  for (auto it = entry_block->rbegin(); it != entry_block->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = it->insn;
    if (opcode::is_load_param(insn->opcode())) {
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

  return live_arg_idxs;
}

/**
 * Returns an updated argument type list for the given method with the given
 * live argument indices.
 */
std::deque<DexType*> RemoveArgs::get_live_arg_type_list(
    DexMethod* method, const std::deque<uint16_t>& live_arg_idxs) {
  std::deque<DexType*> live_args;
  auto args_list = method->get_proto()->get_args()->get_type_list();

  for (uint16_t arg_num : live_arg_idxs) {
    if (!is_static(method)) {
      if (arg_num == 0) {
        continue;
      }
      arg_num--;
    }
    live_args.push_back(args_list.at(arg_num));
  }
  return live_args;
}

/**
 * Returns true on successful update to the given method's signature, where
 * the updated args list is specified by live_args.
 */
bool RemoveArgs::update_method_signature(
    DexMethod* method, const std::deque<uint16_t>& live_arg_idxs) {
  always_assert_log(method->is_def(),
                    "We don't treat virtuals, so methods must be defined\n");
  auto num_orig_args = method->get_proto()->get_args()->get_type_list().size();
  auto live_args = get_live_arg_type_list(method, live_arg_idxs);
  auto live_args_list = DexTypeList::make_type_list(std::move(live_args));
  auto updated_proto =
      DexProto::make_proto(method->get_proto()->get_rtype(), live_args_list);

  auto colliding_method = DexMethod::get_method(
      method->get_class(), method->get_name(), updated_proto);
  if (colliding_method && colliding_method->is_def() &&
      is_constructor(static_cast<const DexMethod*>(colliding_method))) {
    // We can't rename constructors, so we give up on removing args.
    return false;
  }

  auto name = method->get_name();
  if (method->is_virtual()) {
    // TODO: T31388603 -- Remove unused args for true virtuals.

    // We need to worry about creating shadowing in the virtual scope ---
    // for this particular method change, but also across all other upcoming
    // method changes. To this end, we introduce unique names for each name and
    // arg list to avoid any such overlaps.
    size_t name_index = m_renamed_indices[name][live_args_list]++;
    std::stringstream ss;
    // This pass typically runs before the obfuscation pass, so we should not
    // need to be concerned here about creating long method names.
    // "uva" stands for unused virtual args
    ss << name->str() << "$uva" << std::to_string(name_index);
    name = DexString::make_string(ss.str());
  }

  DexMethodSpec spec(method->get_class(), name, updated_proto);
  method->change(spec,
                 true /* rename on collision */,
                 true /* update deobfuscated name */);

  // We must also update debug info when we change the method proto.
  // We calculate this separately from live_args in case the method isn't
  // changeable to avoid unnecessary computation.
  auto code = method->get_code();
  DexDebugItem* debug = nullptr;
  if (code && (debug = code->get_debug_item())) {
    auto& param_names = debug->get_param_names();
    // Avoid adding debug params to methods that don't originally have them.
    // Methods that don't have them are created methods where param debug info
    // seems generally useless anyway.
    if (param_names.size() == num_orig_args) {
      // NOTE: The "this" argument isn't included in param_names, so
      //       we must apply instance_offset to each param_names update.
      size_t instance_offset = is_static(method) ? 0 : 1;
      for (size_t i = instance_offset; i < live_arg_idxs.size(); ++i) {
        param_names.at(i - instance_offset) =
            param_names.at(live_arg_idxs.at(i) - instance_offset);
      }
      param_names.resize(live_arg_idxs.size() - instance_offset);
    }
  }

  TRACE(ARGS, 3, "Method signature updated to %s\n", SHOW(method));
  log_opt(METHOD_PARAMS_REMOVED, method);
  return true;
}

/**
 * For methods that have unused arguments, record live argument registers.
 */
RemoveArgs::MethodStats RemoveArgs::update_meths_with_unused_args() {
  // Phase 1: Find (in parallel) all methods that we can potentially update

  struct Entry {
    std::vector<IRInstruction*> dead_insns;
    std::deque<uint16_t> live_arg_idxs;
  };
  ConcurrentMap<DexMethod*, Entry> unordered_entries;
  walk::parallel::methods(m_scope, [&](DexMethod* method) {
    if (method->get_code() == nullptr) {
      return;
    }
    auto proto = method->get_proto();
    auto num_args = proto->get_args()->size();
    // For instance methods, num_args does not count the 'this' argument.
    if (num_args == 0) {
      // Nothing to do if the method doesn't have args to remove.
      return;
    }

    if (!can_rename(method)) {
      // Nothing to do if ProGuard says we can't change the method args.
      TRACE(ARGS,
            5,
            "Method is disqualified from being updated by ProGuard rules: "
            "%s\n",
            SHOW(method));
      return;
    }

    // If a method is devirtualizable, proceed with live arg computation.
    if (method->is_virtual()) {
      auto virt_scope = m_type_system.find_virtual_scope(method);
      if (virt_scope == nullptr || !is_non_virtual_scope(virt_scope)) {
        // TODO: T31388603 -- Remove unused args for true virtuals.
        return;
      }
    }

    std::vector<IRInstruction*> dead_insns;
    auto live_arg_idxs = compute_live_args(method, num_args, &dead_insns);
    if (dead_insns.empty()) {
      return;
    }

    // Remember entry
    unordered_entries.emplace(method, (Entry){dead_insns, live_arg_idxs});
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
    if (!update_method_signature(method, entry.live_arg_idxs)) {
      continue;
    }

    // Remember entry for further processing, and log statistics
    DexClass* cls = type_class(method->get_class());
    classes.push_back(cls);
    class_entries[cls].push_back(p);
    method_stats.methods_updated_count++;
    method_stats.method_params_removed_count += entry.dead_insns.size();
  }
  sort_unique(classes);

  // Phase 3: Update body of updated methods (in parallel)

  walk::parallel::classes(classes, [&](DexClass* cls) {
    for (auto& p : class_entries.at(cls)) {
      DexMethod* method = p.first;
      const Entry& entry = p.second;

      // We update the method signature, so we must remove unused
      // OPCODE_LOAD_PARAM_* to satisfy IRTypeChecker.
      for (auto dead_insn : entry.dead_insns) {
        method->get_code()->remove_opcode(dead_insn);
      }
      m_live_arg_idxs_map.emplace(method, entry.live_arg_idxs);
    }
  });

  return method_stats;
}

/**
 * Removes dead arguments from the given invoke instr if applicable.
 * Returns the number of arguments removed.
 */
size_t RemoveArgs::update_callsite(IRInstruction* instr) {
  auto method_ref = instr->get_method();
  if (!method_ref->is_def()) {
    // TODO: T31388603 -- Remove unused args for true virtuals.
    return 0;
  };
  DexMethod* method = resolve_method(method_ref, opcode_to_search(instr));

  auto kv_pair = m_live_arg_idxs_map.find(method);
  if (kv_pair == m_live_arg_idxs_map.end()) {
    // No removable arguments, so do nothing.
    return 0;
  }
  auto updated_srcs = kv_pair->second;
  for (size_t i = 0; i < updated_srcs.size(); ++i) {
    instr->set_src(i, instr->src(updated_srcs.at(i)));
  }
  always_assert_log(instr->srcs_size() > updated_srcs.size(),
                    "In RemoveArgs, callsites always update to fewer args\n");
  auto callsite_args_removed = instr->srcs_size() - updated_srcs.size();
  instr->set_arg_word_count(updated_srcs.size());
  return callsite_args_removed;
}

/**
 * Removes unused arguments at callsites and returns the number of arguments
 * removed.
 */
size_t RemoveArgs::update_callsites() {
  // Walk through all methods to look for and edit callsites.
  return walk::parallel::reduce_methods<size_t>(
      m_scope,
      [&](DexMethod* method) -> size_t {
        auto code = method->get_code();
        if (code == nullptr) {
          return 0;
        }
        size_t callsite_args_removed = 0;
        for (const auto& mie : InstructionIterable(code)) {
          auto insn = mie.insn;
          if (is_invoke(insn->opcode())) {
            size_t insn_args_removed = update_callsite(insn);
            if (insn_args_removed > 0) {
              log_opt(CALLSITE_ARGS_REMOVED, method, insn);
              callsite_args_removed += insn_args_removed;
            }
          }
        }
        return callsite_args_removed;
      },
      [](size_t a, size_t b) { return a + b; });
}

void RemoveUnusedArgsPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);

  RemoveArgs rm_args(scope);
  auto pass_stats = rm_args.run();
  size_t num_callsite_args_removed = pass_stats.callsite_args_removed_count;
  size_t num_method_params_removed = pass_stats.method_params_removed_count;
  size_t num_methods_updated = pass_stats.methods_updated_count;

  TRACE(ARGS,
        1,
        "Removed %d redundant callsite arguments\n",
        num_callsite_args_removed);
  TRACE(ARGS,
        1,
        "Removed %d redundant method parameters\n",
        num_method_params_removed);
  TRACE(ARGS,
        1,
        "Updated %d methods with redundant parameters\n",
        num_methods_updated);

  mgr.set_metric(METRIC_CALLSITE_ARGS_REMOVED, num_callsite_args_removed);
  mgr.set_metric(METRIC_METHOD_PARAMS_REMOVED, num_method_params_removed);
  mgr.set_metric(METRIC_METHODS_UPDATED, num_methods_updated);
}

static RemoveUnusedArgsPass s_pass;
} // namespace remove_unused_args
