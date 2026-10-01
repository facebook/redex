/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BaselineProfile.h"

#include <algorithm>
#include <cstddef>
#include <fstream>

#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "MethodUtil.h"
#include "Show.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace baseline_profiles {

bool is_compiled(const DexMethod* method, const MethodFlags& flags) {
  return flags.hot && !method::is_clinit(method);
}

uint32_t dex2oat_code_units(DexMethod* method) {
  auto* code = method->get_code();
  if (code == nullptr) {
    return 0;
  }
  auto code_units = code->estimate_code_units();
  if (code->cfg_built()) {
    code_units += code->cfg().get_size_adjustment();
  }
  return code_units;
}

UncompilableReason uncompilable_reason(DexMethod* method,
                                       uint32_t huge_method_max) {
  if (method->get_code() == nullptr || is_abstract(method) ||
      is_native(method)) {
    return UncompilableReason::kNoCode;
  }
  // Test the access flags rather than the name: dex2oat's gate is
  // ACC_CONSTRUCTOR && ACC_STATIC, and matching it exactly keeps this in step
  // with the compiler even if a name-based helper drifts. An instance <init>
  // is ACC_CONSTRUCTOR without ACC_STATIC and is compiled normally.
  if (is_static(method) && method::is_constructor(method)) {
    return UncompilableReason::kClinit;
  }
  if (has_anno(method, type::dalvik_annotation_optimization_NeverCompile())) {
    return UncompilableReason::kNeverCompile;
  }
  if (dex2oat_code_units(method) > huge_method_max) {
    return UncompilableReason::kHugeMethod;
  }
  return UncompilableReason::kNone;
}

bool flags_request_compilation(const MethodFlags& flags) {
  return flags.hot || flags.startup;
}

TopOffSelection select_smallest_topoff_methods(
    const Scope& candidate_scope,
    const BaselineProfile& baseline_profile,
    size_t count,
    uint32_t huge_method_max) {
  TopOffSelection selection;
  if (count == 0) {
    return selection;
  }

  InsertOnlyConcurrentMap<DexMethod*, uint32_t> eligible;
  walk::parallel::classes(candidate_scope, [&](DexClass* cls) {
    for (auto* method : cls->get_all_methods()) {
      if (baseline_profile.methods.count(method) != 0) {
        continue;
      }
      if (uncompilable_reason(method, huge_method_max) !=
          UncompilableReason::kNone) {
        continue;
      }
      eligible.emplace(method, dex2oat_code_units(method));
    }
  });
  selection.candidates = eligible.size();

  std::vector<std::pair<uint32_t, DexMethod*>> candidates;
  candidates.reserve(eligible.size());
  for (auto&& [method, code_units] : UnorderedIterable(eligible)) {
    candidates.emplace_back(code_units, method);
  }

  if (candidates.size() <= count) {
    // The whole pool is taken, so nothing has to be ranked.
    selection.methods.reserve(candidates.size());
    for (auto&& [code_units, method] : candidates) {
      selection.methods.push_back(method);
    }
    return selection;
  }

  auto by_size = [](const auto& a, const auto& b) { return a.first < b.first; };
  std::nth_element(candidates.begin(),
                   candidates.begin() + static_cast<std::ptrdiff_t>(count - 1),
                   candidates.end(), by_size);
  uint32_t cutoff = candidates[count - 1].first;

  // Everything strictly below the cutoff is in regardless of ordering; only
  // the methods sitting exactly on it need a tie-break, and there are few
  // enough of those to afford building their names.
  std::vector<std::pair<std::string, DexMethod*>> at_cutoff;
  selection.methods.reserve(count);
  for (auto&& [code_units, method] : candidates) {
    if (code_units < cutoff) {
      selection.methods.push_back(method);
    } else if (code_units == cutoff) {
      at_cutoff.emplace_back(show_deobfuscated(method), method);
    }
  }
  // Both follow from nth_element's contract, but a violation would underflow
  // the resize below into an allocation the size of the address space, so pay
  // for the check in opt too.
  always_assert(selection.methods.size() < count);
  size_t remaining = count - selection.methods.size();
  always_assert(at_cutoff.size() >= remaining);
  std::sort(at_cutoff.begin(), at_cutoff.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  at_cutoff.resize(remaining);
  for (auto&& [name, method] : at_cutoff) {
    selection.methods.push_back(method);
  }
  return selection;
}

bool is_compiled(const BaselineProfile& baseline_profile,
                 const DexMethod* method) {
  auto it = baseline_profile.methods.find(method);
  return it != baseline_profile.methods.end() &&
         is_compiled(method, it->second);
}

size_t apply_forced_method_flags(const Scope& scope, BaselineProfile* profile) {
  size_t changed = 0;
  // `walk::methods`, not `walk::code`: the forced H/S bits live on
  // `ReferencedState` and remain meaningful after a method's `IRCode` has been
  // lowered or released. A code-only walk would skip such methods and omit
  // their forced flags from the profile.
  walk::methods(scope, [&](DexMethod* method) {
    const bool force_hot = method->rstate.force_hot_in_baseline_profile();
    const bool force_startup =
        method->rstate.force_startup_in_baseline_profile();
    if (!force_hot && !force_startup) {
      return;
    }
    auto& flags = profile->methods[method];
    // Count bit transitions, not method visits: a method can flip both H and
    // S at once, and the metric is reported per config as a flag count.
    changed += (force_hot && !flags.hot) ? 1 : 0;
    changed += (force_startup && !flags.startup) ? 1 : 0;
    flags.hot |= force_hot;
    flags.startup |= force_startup;
  });
  return changed;
}

BaselineProfile get_default_baseline_profile(
    const Scope& scope,
    const UnorderedMap<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    UnorderedSet<const DexMethodRef*>* method_refs_without_def,
    bool apply_forced_flags) {
  auto [baseline_profile, _] =
      get_baseline_profiles(scope, configs, method_profiles,
                            method_refs_without_def, apply_forced_flags);
  return baseline_profile;
}

std::tuple<BaselineProfile, UnorderedMap<std::string, BaselineProfile>>
get_baseline_profiles(
    const Scope& scope,
    const UnorderedMap<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    UnorderedSet<const DexMethodRef*>* method_refs_without_def,
    bool apply_forced_flags) {
  UnorderedSet<const DexMethodRef*> method_candidates;
  UnorderedSet<DexClass*> class_candidates;
  walk::classes(scope, [&](DexClass* cls) { class_candidates.insert(cls); });
  walk::code(scope, [&](DexMethod* method, const IRCode&) {
    method_candidates.insert(method);
  });
  UnorderedMap<std::string, BaselineProfile> baseline_profiles;
  BaselineProfile manual_baseline_profile;
  for (const auto& [config_name, config] : UnorderedIterable(configs)) {
    // If we're not using this as the final pass of baseline profiles, just
    // continue on all configs that aren't the default
    if (!config.options.use_final_redex_generated_profile &&
        config_name != DEFAULT_BASELINE_PROFILE_CONFIG_NAME) {
      continue;
    }
    UnorderedSet<const DexType*> classes;
    UnorderedSet<const DexMethod*> startup_methods;
    UnorderedSet<const DexMethod*> post_startup_methods;
    UnorderedSet<const DexMethod*> hot_methods;
    for (auto&& [interaction_id, interaction_config] :
         UnorderedIterable(config.interaction_configs)) {
      const auto& method_stats =
          method_profiles.method_stats_for_baseline_config(interaction_id,
                                                           config_name);
      for (auto&& [method_ref, stat] : UnorderedIterable(method_stats)) {
        if (method_candidates.count(method_ref) == 0u) {
          continue;
        }
        const auto* method = method_ref->as_def();
        if (method == nullptr) {
          if (method_refs_without_def != nullptr) {
            method_refs_without_def->insert(method_ref);
          }
          continue;
        }

        if (stat.appear_percent <
                static_cast<double>(interaction_config.threshold) ||
            stat.call_count <
                static_cast<double>(interaction_config.call_threshold)) {
          continue;
        }

        if (interaction_config.startup) {
          startup_methods.insert(method);
          hot_methods.insert(method);
        }
        if (interaction_config.post_startup) {
          post_startup_methods.insert(method);
          hot_methods.insert(method);
        }
        if (interaction_config.classes) {
          classes.insert(method->get_class());
        }
      }
    }
    static const std::array<std::string, 3> manual_interaction_ids = {
        "manual_startup", "manual_post_startup", "manual_hot"};
    for (const auto& interaction_id : manual_interaction_ids) {
      const auto& method_stats =
          method_profiles.method_stats_for_baseline_config(interaction_id,
                                                           config_name);
      for (auto&& [method_ref, stat] : UnorderedIterable(method_stats)) {
        if (method_candidates.count(method_ref) == 0u) {
          continue;
        }
        const auto* method = method_ref->as_def();
        if (method == nullptr) {
          if (method_refs_without_def != nullptr) {
            method_refs_without_def->insert(method_ref);
          }
          continue;
        }
        if (interaction_id == "manual_startup") {
          startup_methods.insert(method);
        }
        if (interaction_id == "manual_post_startup") {
          post_startup_methods.insert(method);
        }
        if (interaction_id == "manual_hot") {
          hot_methods.insert(method);
        }
      }
    }
    // methods = startup_methods | post_startup_methods | hot_methods
    UnorderedSet<const DexMethod*> methods;
    insert_unordered_iterable(methods, startup_methods);
    insert_unordered_iterable(methods, post_startup_methods);
    insert_unordered_iterable(methods, hot_methods);

    baseline_profiles::BaselineProfile res;
    for (const auto* method : UnorderedIterable(methods)) {
      auto& flags = res.methods[method];
      if (startup_methods.count(method) != 0u) {
        flags.startup = true;
      }
      if (post_startup_methods.count(method) != 0u) {
        flags.post_startup = true;
      }
      if (hot_methods.count(method) != 0u) {
        flags.hot = true;
      }
    }
    for (const auto* type : UnorderedIterable(classes)) {
      auto* cls = type_class(type);
      if (class_candidates.count(cls) != 0u) {
        res.classes.insert(type_class(type));
      }
    }
    if (config_name != DEFAULT_BASELINE_PROFILE_CONFIG_NAME ||
        config.options.use_final_redex_generated_profile) {
      baseline_profiles[config_name] = res;
    }
    if (config_name == DEFAULT_BASELINE_PROFILE_CONFIG_NAME) {
      manual_baseline_profile = std::move(res);
    }
  }
  if (apply_forced_flags) {
    apply_forced_method_flags(scope, &manual_baseline_profile);
    for (auto& [_, profile] : UnorderedIterable(baseline_profiles)) {
      apply_forced_method_flags(scope, &profile);
    }
  }
  return {std::move(manual_baseline_profile), std::move(baseline_profiles)};
}

void BaselineProfile::load_classes(const Scope& scope,
                                   const ConfigFiles& config,
                                   const std::string& bp_name) {
  auto preprocessed_profile_name =
      config.get_preprocessed_baseline_profile_file(bp_name);

  if (preprocessed_profile_name.empty()) {
    return;
  }

  // Classes may have been obfuscated. For simplicity create a map
  // ahead of time. We cannot rely on deobfuscated name lookup to be enabled.
  UnorderedMap<std::string_view, DexClass*> unobf_to_type;
  walk::classes(scope, [&](DexClass* cls) {
    const auto* deobf = cls->get_deobfuscated_name_or_null();
    if (deobf != nullptr) {
      unobf_to_type.emplace(deobf->str(), cls);
    } else {
      unobf_to_type.emplace(cls->get_name()->str(), cls);
    }
  });

  std::ifstream preprocessed_profile{preprocessed_profile_name};
  std::string current_line;
  while (std::getline(preprocessed_profile, current_line)) {
    if (current_line.empty() || current_line[0] != 'L') {
      continue;
    }

    auto it = unobf_to_type.find(current_line);
    if (it == unobf_to_type.end() || it->second->is_external()) {
      unmatched_classes.emplace(std::move(current_line));
    } else {
      classes.emplace(it->second);
    }
  }
}

void BaselineProfile::transitively_close_classes(const Scope& scope) {
  // This may not be the most efficient implementation but it is simple and
  // uses common functionality.

  UnorderedSet<const DexType*> closed_types;

  unordered_for_each(classes, [&](auto* cls_def) {
    if (cls_def->is_external()) {
      return;
    }

    cls_def->gather_load_types(closed_types);
  });

  // Filter out classes that are not in the scope. For speed that requires
  // temporary storage. Consider removing this.

  UnorderedSet<const DexClass*> class_defs;
  unordered_for_each(closed_types, [&](auto* type) {
    auto* cls_def = type_class_internal(type);
    if (cls_def == nullptr) {
      return;
    }
    class_defs.emplace(cls_def);
  });

  walk::classes(scope, [&](const auto* cls) {
    if (class_defs.count(cls)) {
      classes.emplace(cls);
    }
  });
}

} // namespace baseline_profiles
