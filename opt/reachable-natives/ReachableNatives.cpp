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
#include "CppUtil.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "IROpcode.h"
#include "LiveRange.h"
#include "PassManager.h"
#include "ProguardConfiguration.h"
#include "Reachability.h"
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Util.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace std::string_literals;

void ReachableNativesPass::bind_config() {
  bind("output_file_name", "redex-reachable-natives.txt", m_output_file_name);
  bind("live_load_library_file_name", "redex-live-load-library.txt",
       m_live_load_library_file_name);
  bind("dead_load_library_file_name", "redex-dead-load-library.txt",
       m_dead_load_library_file_name);
  bind("analyze_load_library", false, m_analyze_load_library);
  bind("additional_load_library_names", {}, m_additional_load_library_names);
  bind("sweep", false, m_sweep);
  bind("sweep_native_methods", false, m_sweep_native_methods);
  after_configuration(
      [this] { always_assert(!m_sweep_native_methods || m_sweep); });
}

bool ReachableNativesPass::gather_load_library(
    DexMethod* caller, InsertOnlyConcurrentSet<const DexString*>* names) {
  if (m_load_library_methods.count(caller) ||
      m_load_library_unsafe_methods.count(caller)) {
    return true;
  }
  cfg::ScopedCFG cfg(caller->get_code());
  Lazy<live_range::UseDefChains> udchain(
      [&]() { return live_range::MoveAwareChains(*cfg).get_use_def_chains(); });
  bool success = true;
  for (auto& mie : InstructionIterable(*cfg)) {
    auto* insn = mie.insn;
    if (!opcode::is_invoke_static(insn->opcode())) {
      continue;
    }
    auto callee = resolve_invoke_method(insn, caller);
    if (!callee) {
      continue;
    }
    if (!m_load_library_methods.count(callee)) {
      continue;
    }
    for (auto* def : (*udchain)[live_range::Use{insn, 0}]) {
      if (opcode::is_const_string(def->opcode())) {
        names->insert(def->get_string());
        continue;
      }
      success = false;
    }
  }
  return success;
}

void ReachableNativesPass::eval_pass(DexStoresVector& stores,
                                     ConfigFiles&,
                                     PassManager&) {
  if (m_eval_number++ > 0) {
    return;
  }
  if (!m_analyze_load_library) {
    return;
  }
  for (std::string_view method_name :
       {"Lcom/facebook/soloader/SoLoader;.loadLibraryUnsafe:(Ljava/lang/"
        "String;)Z",
        "Lcom/facebook/soloader/SoLoader;.loadLibraryUnsafe:(Ljava/lang/"
        "String;I)Z"}) {
    auto* method_ref = DexMethod::get_method(method_name);
    always_assert_log(method_ref, "Did not find method ref %s in input",
                      std::string(method_name).c_str());
    auto* method = method_ref->as_def();
    always_assert_log(method, "Did not find method %s in input",
                      std::string(method_name).c_str());
    always_assert_log(is_static(method), "Expected %s to be static",
                      std::string(method_name).c_str());
    method->rstate.set_root();
    method->rstate.set_dont_inline();
    method->rstate.set_no_outlining();
    m_load_library_unsafe_methods.insert(method);
  }
  for (std::string_view method_name :
       {"Lcom/facebook/soloader/SoLoader;.loadLibrary:(Ljava/lang/String;)Z",
        "Lcom/facebook/soloader/SoLoader;.loadLibrary:(Ljava/lang/String;I)Z",
        "Lcom/facebook/soloader/nativeloader/NativeLoader;.loadLibrary:(Ljava/"
        "lang/String;)Z",
        "Lcom/facebook/soloader/nativeloader/NativeLoader;.loadLibrary:(Ljava/"
        "lang/String;I)Z"}) {
    auto* method_ref = DexMethod::get_method(method_name);
    always_assert_log(method_ref, "Did not find method ref %s in input",
                      std::string(method_name).c_str());
    auto* method = method_ref->as_def();
    always_assert_log(method, "Did not find method %s in input",
                      std::string(method_name).c_str());
    always_assert_log(is_static(method), "Expected %s to be static",
                      std::string(method_name).c_str());
    method->rstate.set_root();
    method->rstate.set_dont_inline();
    method->rstate.set_no_outlining();
    m_load_library_methods.insert(method);
  }

  for (auto& library_name : m_additional_load_library_names) {
    g_redex->library_names.insert(DexString::make_string(library_name));
  }
  InsertOnlyConcurrentSet<DexMethod*> concurrent_non_const_load_library_names;
  walk::parallel::code(
      build_class_scope(stores), [&](DexMethod* caller, IRCode&) {
        if (!gather_load_library(caller, &g_redex->library_names)) {
          concurrent_non_const_load_library_names.insert(caller);
        }
      });
  if (concurrent_non_const_load_library_names.empty()) {
    return;
  }
  std::vector<DexMethod*> ordered(
      concurrent_non_const_load_library_names.begin(),
      concurrent_non_const_load_library_names.end());
  std::sort(ordered.begin(), ordered.end(), compare_dexmethods);
  std::ostringstream oss;
  for (auto* caller : ordered) {
    oss << "  " << show(caller) << "\n";
  }
  always_assert_log(
      false,
      "Found callers of SoLoader.loadLibrary / NativeLoader.loadLibrary that "
      "do not supply a constant library name string:\n%sThis is not supported, "
      "as it prevents identifying which libraries are referenced. Either "
      "change the call to use loadLibraryUnsafe and add possibly library names "
      "to via additional_load_library_names option to the "
      "ReachableNativesPass, or, preferably, refactor the code so that "
      "loadLibrary is called with string constants only.",
      oss.str().c_str());
}

void ReachableNativesPass::analyze_final_load_library(
    const DexClasses& scope,
    ConfigFiles& cfg,
    PassManager& mgr,
    const std::function<bool(DexMethod*)>& reachable_fn) {
  InsertOnlyConcurrentSet<const DexString*> final_library_names;
  for (auto& library_name : m_additional_load_library_names) {
    final_library_names.insert(DexString::make_string(library_name));
  }
  InsertOnlyConcurrentSet<DexMethod*> concurrent_non_const_load_library_names;
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode&) {
    if (!reachable_fn(caller)) {
      return;
    }
    if (!gather_load_library(caller, &final_library_names)) {
      concurrent_non_const_load_library_names.insert(caller);
    }
  });

  // TODO: There is chance that some Redex pass does a code transformation that
  // moves the const-string away, e.g. DedupStringsPass. Consider introducing a
  // "load-library" (pseudo) instruction that takes a string literal, to avoid
  // that.
  always_assert(concurrent_non_const_load_library_names.empty());

  mgr.set_metric("initial_library_names", g_redex->library_names.size());
  mgr.set_metric("final_library_names", final_library_names.size());
  TRACE(NATIVE, 1, "Reachable Library Names: %zu => %zu",
        g_redex->library_names.size(), final_library_names.size());

  for (auto* library_name : final_library_names) {
    always_assert(g_redex->library_names.count(library_name));
  }
  std::vector<const DexString*> ordered(g_redex->library_names.begin(),
                                        g_redex->library_names.end());
  std::sort(ordered.begin(), ordered.end(), compare_dexstrings);

  std::ofstream live_ofs(cfg.metafile(m_live_load_library_file_name),
                         std::ofstream::out | std::ofstream::trunc);
  std::ofstream dead_ofs(cfg.metafile(m_dead_load_library_file_name),
                         std::ofstream::out | std::ofstream::trunc);
  for (auto* library_name : ordered) {
    if (final_library_names.count(library_name)) {
      live_ofs << library_name->str() << "\n";
      TRACE(NATIVE, 2, "live library: %s", library_name->c_str());
    } else {
      dead_ofs << library_name->str() << "\n";
      TRACE(NATIVE, 2, "dead library: %s", library_name->c_str());
    }
  }
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
      scope_set,
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

  if (m_sweep || m_sweep_native_methods) {
    size_t classes_abstracted{0};
    if (!m_sweep_native_methods) {
      // Native methods and their declaring classes themselves must remain
      // reachable, as they may get referenced by native registration code, so
      // we re-include them in the reachable object set, and mark classes as
      // abstract that are only kept for this reason.
      for (auto* m : unreachable_natives) {
        reachable_objects->mark(m);
        self_recursive_fn(
            [&](auto self, DexType* type) {
              auto* cls = type_class(type);
              if (!scope_set.count(cls) ||
                  reachable_objects->marked_unsafe(cls)) {
                return;
              }
              reachable_objects->mark(cls);
              self(self, cls->get_super_class());
              for (auto* intf_type : *cls->get_interfaces()) {
                self(self, intf_type);
              }
              if (!is_abstract(cls)) {
                classes_abstracted++;
                cls->set_access((cls->get_access() & ~ACC_FINAL) |
                                ACC_ABSTRACT);
              }
            },
            m->get_class());
      }
    }

    auto before = reachability::count_objects(stores);
    reachability::sweep(stores, *reachable_objects);
    auto after = reachability::count_objects(stores);

    TRACE(NATIVE, 1, "after: %zu classes, %zu fields, %zu methods",
          after.num_classes, after.num_fields, after.num_methods);
    mgr.incr_metric("classes_removed", before.num_classes - after.num_classes);
    mgr.incr_metric("fields_removed", before.num_fields - after.num_fields);
    mgr.incr_metric("methods_removed", before.num_methods - after.num_methods);
    mgr.incr_metric("classes_abstracted", classes_abstracted);
  }

  if (m_run_number != m_eval_number) {
    return;
  }

  analyze_final_load_library(scope, cfg, mgr, [&](DexMethod* caller) {
    return reachable_objects->marked_unsafe(caller);
  });
}

static ReachableNativesPass s_pass;
