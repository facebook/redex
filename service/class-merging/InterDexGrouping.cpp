/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexGrouping.h"

#include "Model.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

using namespace class_merging;

namespace {

DexType* check_current_instance(const ConstTypeHashSet& types,
                                IRInstruction* insn) {
  DexType* type = nullptr;
  if (insn->has_type()) {
    type =
        const_cast<DexType*>(type::get_element_type_if_array(insn->get_type()));
  } else if (insn->has_method()) {
    type = insn->get_method()->get_class();
  } else if (insn->has_field()) {
    type = insn->get_field()->get_class();
  }

  if (type == nullptr || types.count(type) == 0) {
    return nullptr;
  }

  return type;
}

ConcurrentMap<DexType*, TypeHashSet> get_type_usages(
    const ConstTypeHashSet& types,
    const Scope& scope,
    InterDexGroupingInferringMode mode) {
  TRACE(CLMG, 1, "InterDex Grouping Inferring Mode %s",
        [&]() {
          std::ostringstream oss;
          oss << mode;
          return oss.str();
        }()
            .c_str());
  ConcurrentMap<DexType*, TypeHashSet> res;
  // Ensure all types will be handled.
  for (auto* t : types) {
    res.emplace(const_cast<DexType*>(t), TypeHashSet());
  }

  auto class_loads_update = [&](auto* insn, auto* cls) {
    const auto& updater =
        [&cls](DexType* /* key */, std::unordered_set<DexType*>& set,
               bool /* already_exists */) { set.emplace(cls); };

    if (insn->has_type()) {
      auto current_instance = check_current_instance(types, insn);
      if (current_instance) {
        res.update(current_instance, updater);
      }
    } else if (insn->has_field()) {
      if (opcode::is_an_sfield_op(insn->opcode())) {
        auto current_instance = check_current_instance(types, insn);
        if (current_instance) {
          res.update(current_instance, updater);
        }
      }
    } else if (insn->has_method()) {
      // Load and initialize class for static member access.
      if (opcode::is_invoke_static(insn->opcode())) {
        auto current_instance = check_current_instance(types, insn);
        if (current_instance) {
          res.update(current_instance, updater);
        }
      }
    }
  };

  switch (mode) {
  case InterDexGroupingInferringMode::kClassLoads: {
    walk::parallel::opcodes(scope, [&](DexMethod* method, IRInstruction* insn) {
      auto cls = method->get_class();
      class_loads_update(insn, cls);
    });
    break;
  }

  case InterDexGroupingInferringMode::kClassLoadsBasicBlockFiltering: {
    auto is_not_cold = [](cfg::Block* b) {
      auto* sb = source_blocks::get_first_source_block(b);
      if (sb == nullptr) {
        // Conservatively assume that missing SBs mean no profiling data.
        return true;
      }
      return sb->foreach_val_early(
          [](const auto& v) { return v && v->val > 0; });
    };
    walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
      auto cls = method->get_class();

      cfg::ScopedCFG cfg{&code};

      for (auto* b : cfg->blocks()) {
        // TODO: If we split by interaction, we could check here specifically.
        if (is_not_cold(b)) {
          for (auto& mie : ir_list::InstructionIterable(b)) {
            class_loads_update(mie.insn, cls);
          }
        }
      }
    });
    break;
  }
  }

  return res;
}

size_t get_interdex_group(
    const TypeHashSet& types,
    const std::unordered_map<DexType*, size_t>& cls_to_interdex_groups,
    size_t interdex_groups) {
  // By default, we consider the class in the last group.
  size_t group = interdex_groups - 1;
  for (DexType* type : types) {
    if (cls_to_interdex_groups.count(type)) {
      group = std::min(group, cls_to_interdex_groups.at(type));
    }
  }

  return group;
}

} // namespace

namespace class_merging {

/**
 * Split the types into groups according to the interdex grouping information.
 * Note that types may be dropped if they are not allowed be merged.
 */
void InterDexGrouping::build_interdex_grouping(
    const Scope& scope, const ConstTypeHashSet& merging_targets) {
  const auto& cls_to_interdex_groups = m_conf.get_cls_interdex_groups();
  auto num_interdex_groups = m_conf.get_num_interdex_groups();
  TRACE(CLMG, 5, "num_interdex_groups %zu; cls_to_interdex_groups %zu",
        num_interdex_groups, cls_to_interdex_groups.size());
  size_t num_group = 1;
  if (m_config.is_enabled() && num_interdex_groups > 1) {
    num_group = num_interdex_groups;
  }
  m_all_interdexing_groups = std::vector<ConstTypeHashSet>(num_group);
  if (num_group == 1) {
    m_all_interdexing_groups[0].insert(merging_targets.begin(),
                                       merging_targets.end());
    return;
  }
  const auto& type_to_usages =
      get_type_usages(merging_targets, scope, m_config.inferring_mode);
  for (const auto& pair : type_to_usages) {
    auto index = get_interdex_group(pair.second, cls_to_interdex_groups,
                                    num_interdex_groups);
    if (m_config.type == InterDexGroupingType::NON_HOT_SET) {
      if (index == 0) {
        // Drop mergeables that are in the hot set.
        continue;
      }
    } else if (m_config.type == InterDexGroupingType::NON_ORDERED_SET) {
      if (index < num_interdex_groups - 1) {
        // Only merge the last group which are not in ordered set, drop other
        // mergeables.
        continue;
      }
    }
    m_all_interdexing_groups[index].emplace(pair.first);
  }
}

TypeSet InterDexGrouping::get_types_in_group(const InterdexSubgroupIdx id,
                                             const TypeSet& types) const {
  auto& interdex_group = m_all_interdexing_groups.at(id);
  TypeSet group;
  for (auto* type : types) {
    if (interdex_group.count(type)) {
      group.insert(type);
    }
  }
  return group;
}

void InterDexGrouping::visit_groups(
    const ModelSpec& spec,
    const TypeSet& current_group,
    const std::function<void(const InterdexSubgroupIdx, const TypeSet&)>&
        visit_fn) const {
  for (InterdexSubgroupIdx id = 0; id < m_all_interdexing_groups.size(); id++) {
    if (m_all_interdexing_groups.at(id).empty()) {
      continue;
    }
    auto new_group = this->get_types_in_group(id, current_group);
    if (new_group.size() < spec.min_count) {
      continue;
    }
    visit_fn(id, new_group);
  }
}

void InterDexGroupingConfig::init_type(const std::string& interdex_grouping) {

  const static std::unordered_map<std::string, InterDexGroupingType>
      string_to_grouping = {
          {"disabled", InterDexGroupingType::DISABLED},
          {"non-hot-set", InterDexGroupingType::NON_HOT_SET},
          {"non-ordered-set", InterDexGroupingType::NON_ORDERED_SET},
          {"full", InterDexGroupingType::FULL}};

  always_assert_log(string_to_grouping.count(interdex_grouping) > 0,
                    "InterDex Grouping Type %s not found. Please check the list"
                    " of accepted values.",
                    interdex_grouping.c_str());
  this->type = string_to_grouping.at(interdex_grouping);
}

void InterDexGroupingConfig::init_inferring_mode(const std::string& mode) {
  if (mode.empty()) {
    this->inferring_mode = InterDexGroupingInferringMode::kClassLoads;
  }
  if (mode == "class-loads") {
    this->inferring_mode = InterDexGroupingInferringMode::kClassLoads;
  } else if (mode == "class-loads-bb") {
    this->inferring_mode =
        InterDexGroupingInferringMode::kClassLoadsBasicBlockFiltering;
  } else {
    always_assert_log(false, "Unknown interdex-grouping-inferring-mode %s",
                      mode.c_str());
  }
}

std::ostream& operator<<(std::ostream& os, InterDexGroupingInferringMode mode) {
  switch (mode) {
  case InterDexGroupingInferringMode::kClassLoads:
    os << "class-loads";
    break;
  case InterDexGroupingInferringMode::kClassLoadsBasicBlockFiltering:
    os << "class-loads-bb";
    break;
  }
  return os;
}

} // namespace class_merging
