/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BaselineProfile.h"
#include "StlUtil.h"

namespace baseline_profiles {

BaselineProfile get_baseline_profile(
    const BaselineProfileConfig& config,
    const method_profiles::MethodProfiles& method_profiles,
    std::unordered_set<const DexMethodRef*>* method_refs_without_def) {
  std::unordered_set<const DexType*> classes;
  std::unordered_set<const DexMethod*> startup_methods;
  std::unordered_set<const DexMethod*> post_startup_methods;
  for (auto&& [interaction_id, interaction_config] :
       config.interaction_configs) {
    const auto& method_stats = method_profiles.method_stats(interaction_id);
    for (auto&& [method_ref, stat] : method_stats) {
      auto method = method_ref->as_def();
      if (method == nullptr) {
        if (method_refs_without_def != nullptr) {
          method_refs_without_def->insert(method_ref);
        }
        continue;
      }

      if (stat.appear_percent < interaction_config.threshold ||
          stat.call_count < interaction_config.call_threshold) {
        continue;
      }

      if (interaction_config.startup) {
        startup_methods.insert(method);
      }
      if (interaction_config.post_startup) {
        post_startup_methods.insert(method);
      }
      if (interaction_config.classes) {
        classes.insert(method->get_class());
      }
    }
  }

  // methods = startup_methods | post_startup_methods
  std::unordered_set<const DexMethod*> methods(startup_methods.begin(),
                                               startup_methods.end());
  methods.insert(post_startup_methods.begin(), post_startup_methods.end());

  // startup_post_startup_methods = startup_methods & post_startup_methods
  std::unordered_set<const DexMethod*> startup_post_startup_methods(
      startup_methods.begin(), startup_methods.end());
  std20::erase_if(startup_post_startup_methods, [&](const DexMethod* m) {
    return !post_startup_methods.count(m);
  });

  // startup_methods -= startup_post_startup_methods
  std20::erase_if(startup_methods, [&](const DexMethod* m) {
    return startup_post_startup_methods.count(m);
  });

  // post_startup_methods -= startup_post_startup_methods
  std20::erase_if(post_startup_methods, [&](const DexMethod* m) {
    return startup_post_startup_methods.count(m);
  });

  baseline_profiles::BaselineProfile res;
  for (auto* method : methods) {
    auto& flags = res.methods[method];
    if (startup_post_startup_methods.count(method)) {
      flags.hot = true;
      flags.startup = true;
      flags.post_startup = true;
    } else if (startup_methods.count(method)) {
      flags.hot = true;
      flags.startup = true;
    } else if (post_startup_methods.count(method)) {
      flags.hot = true;
      flags.post_startup = true;
    }
  }
  for (auto* type : classes) {
    res.classes.insert(type_class(type));
  }
  return res;
}

} // namespace baseline_profiles
