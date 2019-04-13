/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SingleImpl.h"

#include <stdio.h>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "Debug.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "SingleImplDefs.h"
#include "SingleImplUtil.h"
#include "Trace.h"
#include "ClassHierarchy.h"
#include "Walkers.h"

size_t SingleImplPass::s_invoke_intf_count = 0;

namespace {

constexpr const char* METRIC_REMOVED_INTERFACES = "num_removed_interfaces";
constexpr const char* METRIC_INVOKE_INT_TO_VIRT = "num_invoke_intf_to_virt";

/**
 * Build a map from interface to the type implementing that
 * interface. We also walk up the interface chain and for every interface
 * in scope (defined in the DEXes) we add an entry as well. So
 * interface B {}
 * interface A extends B {}
 * class C implements A {}
 * generates 2 entries in the map (assuming A, B and C are in the DEXes)
 * { A => C, B => C }
 * whereas if B was outside the DEXes (i.e. java or android interface)
 * we will only have one entry { A => C }
 * keep that in mind when using this map
 */
void map_interfaces(const std::deque<DexType*>& intf_list,
                    DexClass* cls,
                    TypeToTypes& intfs_to_classes) {
  for (auto& intf : intf_list) {
    const auto intf_cls = type_class(intf);
    if (intf_cls == nullptr || intf_cls->is_external()) continue;
    if (std::find(intfs_to_classes[intf].begin(),
                  intfs_to_classes[intf].end(), cls->get_type()) ==
        intfs_to_classes[intf].end()) {
      intfs_to_classes[intf].push_back(cls->get_type());
      auto intfs = intf_cls->get_interfaces();
      map_interfaces(intfs->get_type_list(), cls, intfs_to_classes);
    }
  }
}

/**
 * Collect all interfaces.
 */
void build_type_maps(const Scope& scope,
                     TypeToTypes& intfs_to_classes,
                     TypeSet& interfs) {
  for (const auto& cls : scope) {
    if (is_interface(cls)) {
      interfs.insert(cls->get_type());
      continue;
    }
    auto intfs = cls->get_interfaces();
    map_interfaces(intfs->get_type_list(), cls, intfs_to_classes);
  }
}

void collect_single_impl(const TypeToTypes& intfs_to_classes,
                         TypeMap& single_impl) {
  for (const auto intf_it : intfs_to_classes) {
    if (intf_it.second.size() != 1) continue;
    auto intf = intf_it.first;
    auto intf_cls = type_class(intf);
    always_assert(intf_cls && !intf_cls->is_external());
    if (intf_cls->get_access() & DexAccessFlags::ACC_ANNOTATION) continue;
    auto impl = intf_it.second[0];
    auto impl_cls = type_class(impl);
    always_assert(impl_cls && !impl_cls->is_external());
    // I don't know if it's possible but it's cheap enough to check
    if (impl_cls->get_access() & DexAccessFlags::ACC_ANNOTATION) continue;
    single_impl[intf] = impl;
  }
}

}

const int MAX_PASSES = 8;

void SingleImplPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& mgr) {
  auto scope = build_class_scope(stores);
  ClassHierarchy ch = build_type_hierarchy(scope);
  int max_steps = 0;
  size_t previous_invoke_intf_count = s_invoke_intf_count;
  removed_count = 0;
  const auto& pg_map = conf.get_proguard_map();
  while (true) {
    DEBUG_ONLY size_t scope_size = scope.size();
    TypeToTypes intfs_to_classes;
    TypeSet intfs;
    build_type_maps(scope, intfs_to_classes, intfs);
    TypeMap single_impl;
    collect_single_impl(intfs_to_classes, single_impl);

    std::unique_ptr<SingleImplAnalysis> single_impls =
        SingleImplAnalysis::analyze(
            scope, stores, single_impl, intfs, pg_map, m_pass_config);
    auto optimized = optimize(
        std::move(single_impls), ch, scope, m_pass_config);
    if (optimized == 0 || ++max_steps >= MAX_PASSES) break;
    removed_count += optimized;
    redex_assert(scope_size > scope.size());
  }

  TRACE(INTF, 2, "\ttotal steps %d\n", max_steps);
  TRACE(INTF, 1, "Removed interfaces %ld\n", removed_count);
  TRACE(INTF, 1,
          "Updated invoke-interface to invoke-virtual %ld\n",
          s_invoke_intf_count - previous_invoke_intf_count);

  mgr.incr_metric(METRIC_REMOVED_INTERFACES, removed_count);
  mgr.incr_metric(METRIC_INVOKE_INT_TO_VIRT,
                  s_invoke_intf_count - previous_invoke_intf_count);

  post_dexen_changes(scope, stores);
}

static SingleImplPass s_pass;
