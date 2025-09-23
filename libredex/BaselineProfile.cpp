/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BaselineProfile.h"

#include <fstream>

#include "ConfigFiles.h"
#include "Walkers.h"

namespace baseline_profiles {

BaselineProfile get_default_baseline_profile(
    const Scope& scope,
    const UnorderedMap<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    UnorderedSet<const DexMethodRef*>* method_refs_without_def) {
  auto [baseline_profile, _] = get_baseline_profiles(
      scope, configs, method_profiles, method_refs_without_def);
  return baseline_profile;
}

std::tuple<BaselineProfile, UnorderedMap<std::string, BaselineProfile>>
get_baseline_profiles(
    const Scope& scope,
    const UnorderedMap<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    UnorderedSet<const DexMethodRef*>* method_refs_without_def) {
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
  // ahead of time. We cannot rely on debofuscated name lookup to be enabled.
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

  UnorderedSet<DexType*> closed_types;

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
