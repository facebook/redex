/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisAwareRemoveUnreachable.h"

#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Reachability.h"
#include "Show.h"
#include "Timer.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace reachability;
namespace mog = method_override_graph;

namespace {

class TypeAnaysisAwareClosureMarker final
    : public reachability::TransitiveClosureMarker {
 public:
  explicit TypeAnaysisAwareClosureMarker(
      const IgnoreSets& ignore_sets,
      const method_override_graph::Graph& method_override_graph,
      bool record_reachability,
      ConditionallyMarked* cond_marked,
      ReachableObjects* reachable_objects,
      MarkWorkerState* worker_state,
      Stats* stats)
      : reachability::TransitiveClosureMarker(ignore_sets,
                                              method_override_graph,
                                              record_reachability,
                                              cond_marked,
                                              reachable_objects,
                                              worker_state,
                                              stats) {}
  // The overriding methods below are identical to their base implementation.
  // They are here just to scaffold overall structure and will be updated in the
  // next diff.
  References gather(const DexMethod* method) const override {
    References refs;
    method->gather_strings(refs.strings);
    method->gather_types(refs.types);
    method->gather_fields(refs.fields);
    method->gather_methods(refs.methods);
    return refs;
  }

  void visit_method_ref(const DexMethodRef* method) override {
    TRACE(REACH, 4, "Visiting method: %s", SHOW(method));
    auto resolved_method =
        resolve_without_context(method, type_class(method->get_class()));
    if (resolved_method != nullptr) {
      TRACE(REACH, 5, "    Resolved to: %s", SHOW(resolved_method));
      this->push(method, resolved_method);
      gather_and_push(resolved_method);
    }
    push(method, method->get_class());
    push(method, method->get_proto()->get_rtype());
    for (auto const& t : method->get_proto()->get_args()->get_type_list()) {
      push(method, t);
    }
    auto m = method->as_def();
    if (!m) {
      return;
    }
    // If we're keeping an interface or virtual method, we have to keep its
    // implementations and overriding methods respectively.
    if (m->is_virtual() || !m->is_concrete()) {
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, m);
      for (auto* overriding : overriding_methods) {
        push_cond(overriding);
      }
    }
  }
};

std::unique_ptr<ReachableObjects> compute_reachable_objects_with_type_anaysis(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings) {
  Timer t("Marking");
  auto scope = build_class_scope(stores);
  auto reachable_objects = std::make_unique<ReachableObjects>();
  ConditionallyMarked cond_marked;
  auto method_override_graph = mog::build_graph(scope);
  bool record_reachability = false;

  ConcurrentSet<ReachableObject, ReachableObjectHash> root_set;
  RootSetMarker root_set_marker(*method_override_graph,
                                record_reachability,
                                &cond_marked,
                                reachable_objects.get(),
                                &root_set);
  root_set_marker.mark(scope);

  size_t num_threads = redex_parallel::default_num_threads();
  auto stats_arr = std::make_unique<Stats[]>(num_threads);
  auto work_queue = workqueue_foreach<ReachableObject>(
      [&](MarkWorkerState* worker_state, const ReachableObject& obj) {
        TypeAnaysisAwareClosureMarker transitive_closure_marker(
            ignore_sets, *method_override_graph, record_reachability,
            &cond_marked, reachable_objects.get(), worker_state,
            &stats_arr[worker_state->worker_id()]);
        transitive_closure_marker.visit(obj);
        return nullptr;
      },
      num_threads,
      /*push_tasks_while_running=*/true);
  for (const auto& obj : root_set) {
    work_queue.add_item(obj);
  }
  work_queue.run_all();

  if (num_ignore_check_strings != nullptr) {
    for (size_t i = 0; i < num_threads; ++i) {
      *num_ignore_check_strings += stats_arr[i].num_ignore_check_strings;
    }
  }

  return reachable_objects;
}

} // namespace

void TypeAnalysisAwareRemoveUnreachablePass::run_pass(DexStoresVector& stores,
                                                      ConfigFiles&,
                                                      PassManager& pm) {
  // Store names of removed classes and methods
  ConcurrentSet<std::string> removed_symbols;

  if (pm.no_proguard_rules()) {
    TRACE(RMU,
          1,
          "TypeAnalysisAwareRemoveUnreachablePass not run because no "
          "ProGuard configuration was provided.");
    return;
  }

  bool output_unreachable_symbols = pm.get_current_pass_info()->repeat == 0;
  int num_ignore_check_strings = 0;
  auto reachables = compute_reachable_objects_with_type_anaysis(
      stores, m_ignore_sets, &num_ignore_check_strings);

  reachability::ObjectCounts before = reachability::count_objects(stores);
  TRACE(RMU, 1, "before: %lu classes, %lu fields, %lu methods",
        before.num_classes, before.num_fields, before.num_methods);
  pm.set_metric("before.num_classes", before.num_classes);
  pm.set_metric("before.num_fields", before.num_fields);
  pm.set_metric("before.num_methods", before.num_methods);
  pm.set_metric("marked_classes", reachables->num_marked_classes());
  pm.set_metric("marked_fields", reachables->num_marked_fields());
  pm.set_metric("marked_methods", reachables->num_marked_methods());

  reachability::sweep(stores, *reachables,
                      output_unreachable_symbols ? &removed_symbols : nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);
  TRACE(RMU, 1, "after: %lu classes, %lu fields, %lu methods",
        after.num_classes, after.num_fields, after.num_methods);
  pm.incr_metric("num_ignore_check_strings", num_ignore_check_strings);
  pm.incr_metric("classes_removed", before.num_classes - after.num_classes);
  pm.incr_metric("fields_removed", before.num_fields - after.num_fields);
  pm.incr_metric("methods_removed", before.num_methods - after.num_methods);
}

static TypeAnalysisAwareRemoveUnreachablePass s_pass;
