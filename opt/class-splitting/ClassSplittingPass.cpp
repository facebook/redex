/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass splits out methods that are not frequently called (see
 * method_profiles_appear_percent_threshold for the frequent threashold) from
 * the cold-start dexes.
 *
 * The approach here is a new interdex plugin (with the possibility of running
 * it outside InterDex as well). This enables:
 * - only treating classes that end up in the non-primary cold-start dexes;
 * - accounting for extra classes, which is important to determine when a dex
 *   is full.
 *
 * Relocated methods are moved into new special classes. Each class is filled
 * with up to a configurable number of methods; only when a class is full,
 * another one is created. Separate classes might be created for distinct
 * required api levels.
 */

#include "ClassSplittingPass.h"

#include <algorithm>
#include <boost/functional/hash.hpp>
#include <vector>

#include "ApiLevelChecker.h"
#include "ClassSplitting.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPass.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "PluginRegistry.h"
#include "Resolver.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

using namespace interdex;

namespace class_splitting {

void ClassSplittingPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  TRACE(CS, 1, "[class splitting] Enabled: %d", m_config.enabled);
  if (!m_config.enabled) {
    return;
  }

  const auto& method_profiles = conf.get_method_profiles();
  if (!method_profiles.has_stats()) {
    TRACE(CS, 1,
          "[class splitting] Disabled since we don't have method profiles");
    return;
  }

  // We are going to simulate how the InterDex pass would invoke our plug-in in
  // a way that can run before the actual InterDex pass. Then, the actual
  // InterDex pass run can reshuffle the split-off classes across dexes
  // properly, accounting for all the changes to refs from the beginning.

  std::unordered_set<DexMethod*> sufficiently_popular_methods;
  // Methods that appear in the profiles and whose frequency does not exceed
  // the threashold.
  std::unordered_set<DexMethod*> insufficiently_popular_methods;

  Scope scope = build_class_scope(stores);
  for (auto& p : method_profiles.all_interactions()) {
    auto& method_stats = p.second;
    walk::methods(scope, [&](DexMethod* method) {
      auto it = method_stats.find(method);
      if (it == method_stats.end()) {
        return;
      }
      if (it->second.appear_percent >=
          m_config.method_profiles_appear_percent_threshold) {
        sufficiently_popular_methods.insert(method);
      } else {
        insufficiently_popular_methods.insert(method);
      }
    });
  }

  ClassSplitter class_splitter(m_config, mgr, sufficiently_popular_methods,
                               insufficiently_popular_methods);
  class_splitter.configure(scope);
  std::unordered_set<DexType*> coldstart_types;
  std::vector<std::string> previously_relocated_types;
  for (const auto& str : conf.get_coldstart_classes()) {
    DexType* type = DexType::get_type(str);
    if (type) {
      coldstart_types.insert(type);
    } else if (boost::algorithm::ends_with(
                   str, CLASS_SPLITTING_RELOCATED_SUFFIX_SEMI)) {
      previously_relocated_types.emplace_back(str);
    }
  }

  // Since classes that we previously split and ONLY the relocated part
  // appears
  // in coldstart types won't be actually split this time, we also need to
  // update the initial class ordering to reflect that.
  update_coldstart_classes_order(conf, mgr, coldstart_types,
                                 previously_relocated_types);

  // In a clandestine way, we create instances of all InterDex plugins on the
  // side in order to check if we should skip a class for some obscure
  // reason.
  interdex::InterDexRegistry* registry =
      static_cast<interdex::InterDexRegistry*>(
          PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
  auto plugins = registry->create_plugins();

  TRACE(CS, 2,
        "[class splitting] Operating on %zu cold-start types and %zu plugins",
        coldstart_types.size(), plugins.size());

  auto should_skip = [&](DexClass* cls) {
    for (auto& plugin : plugins) {
      if (plugin->should_skip_class(cls)) {
        return true;
      }
    }
    return false;
  };

  // We are only going to perform class-splitting in the first store, as
  // that's
  // where all the perf-sensitive classes are.
  auto& store = stores.at(0);
  auto& dexen = store.get_dexen();
  DexClasses classes;
  // We skip the first dex, as that's the primary dex, and we won't split
  // classes in there anyway.
  for (size_t dex_nr = 1; dex_nr < dexen.size(); dex_nr++) {
    auto& dex = dexen.at(dex_nr);
    for (auto cls : dex) {
      if (!coldstart_types.count(cls->get_type()) &&
          !cls->rstate.has_interdex_subgroup()) {
        continue;
      }
      if (should_skip(cls)) {
        continue;
      }
      classes.push_back(cls);
      class_splitter.prepare(cls, nullptr /* mrefs */, nullptr /* trefs */);
    }
  }
  auto classes_to_add = class_splitter.additional_classes(classes);
  dexen.push_back(classes_to_add);
  TRACE(CS, 1, "[class splitting] Added %zu classes", classes_to_add.size());
  auto final_scope = build_class_scope(stores);
  class_splitter.cleanup(final_scope);
}

static ClassSplittingPass s_pass;
} // namespace class_splitting
