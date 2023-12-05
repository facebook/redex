/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReachableNatives.h"

#include <algorithm>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/optional/optional.hpp>
#include <boost/range/adaptor/map.hpp>
#include <fstream>
#include <iterator>
#include <string>

#include "BinarySerialization.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "ProguardConfiguration.h"
#include "Reachability.h"
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "Resolver.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Util.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace std::string_literals;

void ReachableNativesPass::bind_config() {
  bind("output_file_name", "redex-reachable-natives.txt", m_output_file_name);
}

void ReachableNativesPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  m_run_number++;
  const auto& file_name = cfg.metafile(m_output_file_name);
  auto trace_opts = [this]() {
    if (m_run_number == 1) {
      return std::ofstream::out | std::ofstream::trunc;
    }
    return std::ofstream::out | std::ofstream::app;
  }();

  std::ofstream ofs(file_name, trace_opts);

  auto log_line = [&ofs](const auto& line) {
    TRACE(NATIVE, 2, "%s", line);
    ofs << line << "\n";
  };

  log_line(
      ("ReachableNativesPass Run "s + std::to_string(m_run_number)).c_str());

  auto scope = build_class_scope(stores);
  std::unordered_set<const DexClass*> scope_set(scope.begin(), scope.end());
  auto reachable_objects = std::make_unique<reachability::ReachableObjects>();
  reachability::ReachableAspects reachable_aspects;
  reachability::ConditionallyMarked cond_marked;
  auto method_override_graph = method_override_graph::build_graph(scope);

  ConcurrentSet<reachability::ReachableObject,
                reachability::ReachableObjectHash>
      root_set;
  reachability::RootSetMarker root_set_marker(
      *method_override_graph, false, false, false, &cond_marked,
      reachable_objects.get(), &root_set);

  TRACE(NATIVE, 2, "Blanket Native Classes: %zu",
        g_redex->blanket_native_root_classes.size());
  TRACE(NATIVE, 2, "Blanket Native Methods: %zu",
        g_redex->blanket_native_root_methods.size());

  root_set_marker.mark_with_exclusions(scope,
                                       g_redex->blanket_native_root_classes,
                                       g_redex->blanket_native_root_methods);

  size_t num_threads = redex_parallel::default_num_threads();
  reachability::IgnoreSets ignore_sets;
  reachability::Stats stats;
  reachability::TransitiveClosureMarkerSharedState shared_state{
      std::move(scope_set),
      &ignore_sets,
      method_override_graph.get(),
      false,
      false,
      false,
      false,
      false,
      false,
      &cond_marked,
      reachable_objects.get(),
      &reachable_aspects,
      &stats};
  workqueue_run<reachability::ReachableObject>(
      [&](reachability::TransitiveClosureMarkerWorkerState* worker_state,
          const reachability::ReachableObject& obj) {
        reachability::TransitiveClosureMarkerWorker worker(&shared_state,
                                                           worker_state);
        worker.visit(obj);
        return nullptr;
      },
      root_set, num_threads,
      /* push_tasks_while_running */ true);
  compute_zombie_methods(*method_override_graph, *reachable_objects,
                         reachable_aspects);

  std::unordered_set<DexMethod*> reachable_natives;
  std::unordered_set<DexMethod*> unreachable_natives;

  walk::methods(scope, [&](DexMethod* m) {
    if (is_native(m)) {
      if (reachable_objects->marked_unsafe(static_cast<DexMethodRef*>(m))) {
        log_line(SHOW(m));
        reachable_natives.insert(m);
      } else {
        unreachable_natives.insert(m);
      }
    }
  });

  log_line("Native methods reachable from non-native:");
  for (auto* m : reachable_natives) {
    log_line(SHOW(m));
  }
  log_line("");

  log_line("Native methods unreachable from non-native:");
  for (auto* m : unreachable_natives) {
    log_line(SHOW(m));
  }
  log_line("");

  TRACE(NATIVE, 1, "Reachable Natives: %zu, Unreachable Natives: %zu",
        reachable_natives.size(), unreachable_natives.size());

  mgr.set_metric("reachable_natives", reachable_natives.size());
  mgr.set_metric("unreachable_natives", unreachable_natives.size());
}

static ReachableNativesPass s_pass;
