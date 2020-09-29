/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodMerger.h"

#include "DexAsm.h"
#include "MethodOverrideGraph.h"
#include "MethodReference.h"
#include "Resolver.h"
#include "SwitchDispatch.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

std::vector<DexMethod*> get_static_methods(
    const std::vector<DexMethod*>& dmethods) {
  std::vector<DexMethod*> methods;
  for (auto method : dmethods) {
    if (!is_static(method)) {
      continue;
    }
    methods.push_back(method);
  }
  return methods;
}
std::vector<DexMethod*> get_direct_instance_methods(
    const std::vector<DexMethod*>& dmethods) {
  std::vector<DexMethod*> methods;
  for (auto method : dmethods) {
    if (is_static(method) || method::is_constructor(method)) {
      continue;
    }
    methods.push_back(method);
  }
  return methods;
}

std::unordered_set<DexMethod*> methodgroups_to_methodset(
    const method_merger::MethodGroups& method_groups) {
  std::unordered_set<DexMethod*> method_set;
  for (auto& methods : method_groups) {
    method_set.insert(methods.begin(), methods.end());
  }
  return method_set;
}

/**
 * Count callsites of candidate methods.
 */
class RefCounter {
 public:
  explicit RefCounter(method_reference::CallSites& call_sites) {
    for (auto& callsite : call_sites) {
      ++m_counter[callsite.callee];
    }
  }
  bool too_less_callers(DexMethod* method) const {
    if (!m_counter.count(method)) {
      return true;
    }
    return m_counter.at(method) < 2;
  }

 private:
  std::unordered_map<DexMethod*, uint32_t> m_counter;
};

void create_one_dispatch(
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee,
    uint32_t min_size,
    std::unordered_map<DexMethod*, method_reference::NewCallee>* old_to_new,
    method_merger::Stats* stats) {
  if (indices_to_callee.size() < min_size) {
    return;
  }
  auto first_method = indices_to_callee.begin()->second;
  auto method = dispatch::create_simple_dispatch(indices_to_callee);
  always_assert_log(method != nullptr, "Dispatch null for %s\n",
                    SHOW(first_method));
  auto cls = type_class(first_method->get_class());
  cls->add_method(method);
  for (auto& id_meth : indices_to_callee) {
    uint32_t tag = *id_meth.first.begin();
    method_reference::NewCallee new_callee(method, tag);
    old_to_new->emplace(id_meth.second, std::move(new_callee));
  }
  // Record stats: number of merged methods - number of dispatches.
  uint32_t merged_size = indices_to_callee.size() - 1;
  if (first_method->is_virtual()) {
    stats->num_merged_nonvirt_methods += merged_size;
  } else if (is_static(first_method)) {
    stats->num_merged_static_methods += merged_size;
  } else {
    stats->num_merged_direct_methods += merged_size;
  }
}

/**
 * Generate dispatches for the methods, then update old_to_new mapping and
 * update stats.
 */
void generate_dispatches(
    const std::vector<DexMethod*>& methods,
    const RefCounter& ref_counter,
    std::unordered_map<DexMethod*, method_reference::NewCallee>* old_to_new,
    method_merger::Stats* stats) {
  constexpr uint64_t HARD_MAX_INSTRUCTION_SIZE = 1L << 16;
  constexpr uint32_t min_method_group_size = 3;
  std::unordered_map<DexProto*, std::set<DexMethod*, dexmethods_comparator>>
      proto_to_methods;
  for (auto method : methods) {
    // Use dispatch::may_be_dispatch(method) to heuristically exclude large
    // dispatches.
    if (!root(method) && can_rename(method) &&
        !ref_counter.too_less_callers(method) &&
        !dispatch::may_be_dispatch(method)) {
      proto_to_methods[method->get_proto()].insert(method);
    }
  }
  for (auto& p : proto_to_methods) {
    if (p.second.size() < min_method_group_size) {
      continue;
    }
    std::map<SwitchIndices, DexMethod*> indices_to_callee;
    uint64_t code_size = 0;
    uint32_t id = 0;
    for (auto it = p.second.begin(); it != p.second.end(); ++it) {
      auto cur_meth = *it;
      code_size += cur_meth->get_code()->sum_opcode_sizes();
      if (code_size > HARD_MAX_INSTRUCTION_SIZE) {
        create_one_dispatch(indices_to_callee, min_method_group_size,
                            old_to_new, stats);
        indices_to_callee.clear();
        code_size = 0;
        id = 0;
      }
      SwitchIndices indices{static_cast<int>(id)};
      indices_to_callee[indices] = cur_meth;
      ++id;
    }
    create_one_dispatch(indices_to_callee, min_method_group_size, old_to_new,
                        stats);
  }
}
} // namespace

namespace method_merger {

Stats merge_methods(const MethodGroups& method_groups,
                    const DexClasses& scope) {
  Stats stats;
  auto all_methods = methodgroups_to_methodset(method_groups);
  if (all_methods.empty()) {
    return stats;
  }
  method_reference::CallSites callsites =
      method_reference::collect_call_refs(scope, all_methods);
  RefCounter ref_counter(callsites);
  std::unordered_map<DexMethod*, method_reference::NewCallee> old_to_new;
  for (auto& methods : method_groups) {
    generate_dispatches(methods, ref_counter, &old_to_new, &stats);
  }
  if (old_to_new.empty()) {
    return stats;
  }
  for (auto& callsite : callsites) {
    auto old_callee = callsite.callee;
    if (!old_to_new.count(old_callee)) {
      continue;
    }
    auto& new_callee = old_to_new.at(old_callee);
    method_reference::patch_callsite(callsite, new_callee);
    TRACE(METH_MERGER, 9, "\t%s => %d %s", SHOW(old_callee),
          new_callee.additional_args.get()[0], SHOW(new_callee.method));
  }
  if (traceEnabled(METH_MERGER, 3)) {
    TRACE(METH_MERGER, 3, "merged static methods : %u",
          stats.num_merged_static_methods);
    TRACE(METH_MERGER, 3, "merged direct methods : %u",
          stats.num_merged_direct_methods);
    TRACE(METH_MERGER, 3, "merged virtual methods : %u",
          stats.num_merged_nonvirt_methods);
  }
  return stats;
}

Stats merge_methods_within_class(const DexClasses& classes,
                                 const DexClasses& scope,
                                 bool merge_static,
                                 bool merge_non_virtual,
                                 bool merge_direct) {
  MethodGroups method_groups;
  if (merge_non_virtual) {
    std::unordered_map<DexType*, std::vector<DexMethod*>> methods;
    std::for_each(classes.begin(), classes.end(),
                  [&methods](DexClass* clazz) { methods[clazz->get_type()]; });
    auto non_virtuals = mog::get_non_true_virtuals(scope);
    std::for_each(non_virtuals.begin(), non_virtuals.end(),
                  [&methods](DexMethod* method) {
                    auto type = method->get_class();
                    if (!methods.count(type) || !can_rename(method) ||
                        root(method)) {
                      return;
                    }
                    methods[type].push_back(method);
                  });
    for (auto& p : methods) {
      method_groups.push_back(std::move(p.second));
    }
  }
  if (merge_static || merge_direct) {
    std::function<void(DexClass*)> add_statics = [](DexClass* cls) {};
    if (merge_static) {
      add_statics = [&method_groups](DexClass* cls) {
        auto methods = get_static_methods(cls->get_dmethods());
        method_groups.push_back(std::move(methods));
      };
    }
    std::function<void(DexClass*)> add_directs = [](DexClass* cls) {};
    if (merge_direct) {
      add_directs = [&method_groups](DexClass* cls) {
        auto methods = get_direct_instance_methods(cls->get_dmethods());
        method_groups.push_back(methods);
      };
    }
    for (auto cls : classes) {
      add_statics(cls);
      add_directs(cls);
    }
  }
  return merge_methods(method_groups, scope);
}
} // namespace method_merger
