/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnreachable.h"

#include <atomic>
#include <fstream>
#include <set>

#include "ConfigFiles.h"
#include "DexUtil.h"
#include "IOUtil.h"
#include "InitClassesWithSideEffects.h"
#include "LocalDce.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"

bool RemoveUnreachablePassBase::s_emit_graph_on_last_run{false};
size_t RemoveUnreachablePassBase::s_all_reachability_runs{0};
size_t RemoveUnreachablePassBase::s_all_reachability_run{0};

namespace {
const std::string UNREACHABLE_SYMBOLS_FILENAME =
    "redex-unreachable-removed-symbols.txt";
const std::string REMOVED_SYMBOLS_REFERENCES_FILENAME =
    "redex-unreachable-removed-symbols-references.txt";
const std::string RMU_PASS_NAME = "RemoveUnreachablePass";

void root_metrics(DexStoresVector& stores, PassManager& pm) {
  auto scope = build_class_scope(stores);
  std::atomic<size_t> root_classes{0};
  std::atomic<size_t> root_methods{0};
  std::atomic<size_t> root_fields{0};

  walk::parallel::classes(scope, [&](const DexClass* cls) {
    if (root(cls)) {
      root_classes++;
    }

    for (auto const& f : cls->get_ifields()) {
      if (root(f)) {
        root_fields++;
      }
    }
    for (auto const& f : cls->get_sfields()) {
      if (root(f)) {
        root_fields++;
      }
    }

    for (auto const& m : cls->get_dmethods()) {
      if (root(m)) {
        root_methods++;
      }
    }
    for (auto const& m : cls->get_dmethods()) {
      if (root(m)) {
        root_methods++;
      }
    }
  });
  pm.set_metric("root_classes", root_classes.load());
  pm.set_metric("root_methods", root_methods.load());
  pm.set_metric("root_fields", root_fields.load());
}

using ConcurrentReferencesMap =
    ConcurrentMap<std::string, std::unordered_set<std::string>>;

template <class Container>
void update_references(Container& c,
                       std::unordered_set<std::string>& references) {
  for (auto i = c.begin(); i != c.end(); i++) {
    references.insert(show_deobfuscated(*i));
  }
}

template <class Container>
void gather_references(const reachability::ReachableObjects& reachables,
                       Container* c,
                       ConcurrentReferencesMap& references) {
  auto p = [&](const auto& m) {
    if (reachables.marked_unsafe(m) == 0) {
      return false;
    }
    return true;
  };
  const auto it = std::partition(c->begin(), c->end(), p);

  for (auto elem = it; elem != c->end(); elem++) {
    std::unordered_set<std::string> current_references;

    std::unordered_set<DexMethodRef*> mrefs;
    (*elem)->gather_methods(mrefs);

    std::unordered_set<DexFieldRef*> frefs;
    (*elem)->gather_fields(frefs);

    std::unordered_set<DexType*> trefs;
    (*elem)->gather_types(trefs);

    update_references(mrefs, current_references);
    update_references(frefs, current_references);
    update_references(trefs, current_references);
    references.emplace(show_deobfuscated(*elem), current_references);
  }
}

void gather_references_from_removed_symbols(
    const DexStoresVector& stores,
    const reachability::ReachableObjects& reachables,
    ConcurrentReferencesMap& references) {
  for (auto& dex : DexStoreClassesIterator(stores)) {
    gather_references(reachables, &dex, references);
    walk::parallel::classes(dex, [&](DexClass* cls) {
      gather_references(reachables, &cls->get_ifields(), references);
      gather_references(reachables, &cls->get_sfields(), references);
      gather_references(reachables, &cls->get_dmethods(), references);
      gather_references(reachables, &cls->get_vmethods(), references);
    });
  }
}

void write_out_removed_symbols_references(
    const std::string& filepath,
    const ConcurrentSet<std::string>& removed_symbols,
    const ConcurrentReferencesMap& references) {
  std::fstream out(filepath, std::ios_base::app);
  if (!out.is_open()) {
    fprintf(stderr,
            "Unable to write the removed symbols references into file %s\n",
            filepath.c_str());
    return;
  }
  TRACE(RMU, 4, "Writing %zu removed symbols references to %s",
        removed_symbols.size(), filepath.c_str());

  struct StringPtrComparator {
    bool operator()(const std::string* s1, const std::string* s2) const {
      return *s1 < *s2;
    }
  };
  std::set<const std::string*, StringPtrComparator> sorted;
  unordered_transform(removed_symbols,
                      std::inserter(sorted, sorted.end()),
                      [](const std::string& s) { return &s; });

  std::unordered_map<std::string, std::unordered_set<std::string>>
      referenced_to_references;
  for (const auto& pair : UnorderedIterable(references)) {
    for (const auto& elem : pair.second) {
      referenced_to_references[elem].emplace(pair.first);
    }
  }

  for (auto s_ptr : sorted) {
    if (referenced_to_references.count(*s_ptr) == 0) {
      continue;
    }

    out << *s_ptr << std::endl;
    for (const auto& ref : referenced_to_references.at(*s_ptr)) {
      out << "\t" << ref << std::endl;
    }
  }
}

} // namespace

namespace mog = method_override_graph;

reachability::ObjectCounts RemoveUnreachablePassBase::before_metrics(
    DexStoresVector& stores, PassManager& pm) {
  reachability::ObjectCounts before = reachability::count_objects(stores);
  TRACE(RMU, 1, "before: %zu classes, %zu fields, %zu methods",
        before.num_classes, before.num_fields, before.num_methods);
  pm.set_metric("before.num_classes", before.num_classes);
  pm.set_metric("before.num_fields", before.num_fields);
  pm.set_metric("before.num_methods", before.num_methods);
  return before;
}

void RemoveUnreachablePassBase::bind_config() {
  bind("ignore_string_literals", {}, m_ignore_sets.string_literals);
  bind("ignore_string_literal_annos", {}, m_ignore_sets.string_literal_annos);
  bind("keep_class_in_string", true, m_ignore_sets.keep_class_in_string);
  bind("emit_graph_on_run", std::optional<uint32_t>{}, m_emit_graph_on_run);
  bool emit_on_last{false};
  bind("emit_graph_on_last_run", emit_on_last, emit_on_last);
  bind("always_emit_unreachable_symbols",
       false,
       m_always_emit_unreachable_symbols);
  // This config allows unused constructors without argument to be removed.
  // This is only used for testing in microbenchmarks.
  bind("remove_no_argument_constructors",
       false,
       m_remove_no_argument_constructors);
  bind("output_full_removed_symbols", false, m_output_full_removed_symbols);
  bind("relaxed_keep_class_members", false, m_relaxed_keep_class_members);
  bind("prune_uninstantiable_insns", false, m_prune_uninstantiable_insns);
  bind("prune_uncallable_instance_method_bodies",
       false,
       m_prune_uncallable_instance_method_bodies);
  bind("prune_uncallable_virtual_methods",
       false,
       m_prune_uncallable_virtual_methods);
  bind("prune_unreferenced_interfaces", false, m_prune_unreferenced_interfaces);
  bind("throw_propagation", false, m_throw_propagation);
  after_configuration([emit_on_last]() {
    if (emit_on_last) {
      s_emit_graph_on_last_run = true;
    }
  });
}

void RemoveUnreachablePassBase::eval_pass(DexStoresVector& /*stores*/,
                                          ConfigFiles& /*conf*/,
                                          PassManager& /*mgr*/) {
  ++s_all_reachability_runs;
}

void RemoveUnreachablePassBase::run_pass(DexStoresVector& stores,
                                         ConfigFiles& conf,
                                         PassManager& pm) {
  ++s_all_reachability_run;

  // Store names of removed classes and methods
  ConcurrentSet<std::string> removed_symbols;

  auto sweep_code = should_sweep_code();
  auto scope = build_class_scope(stores);
  always_assert(!pm.unreliable_virtual_scopes());
  auto method_override_graph = mog::build_graph(scope);
  std::unique_ptr<init_classes::InitClassesWithSideEffects>
      init_classes_with_side_effects;
  if (sweep_code && !pm.init_class_lowering_has_run()) {
    init_classes_with_side_effects =
        std::make_unique<init_classes::InitClassesWithSideEffects>(
            scope, conf.create_init_class_insns(), method_override_graph.get());
  }

  root_metrics(stores, pm);
  auto before = before_metrics(stores, pm);
  bool emit_graph_this_run =
      (m_emit_graph_on_run &&
       static_cast<int64_t>(pm.get_current_pass_info()->repeat + 1) ==
           *m_emit_graph_on_run) ||
      (s_emit_graph_on_last_run &&
       s_all_reachability_runs == s_all_reachability_run);
  bool output_unreachable_symbols =
      m_always_emit_unreachable_symbols ||
      (pm.get_current_pass_info()->repeat == 0 &&
       pm.get_current_pass_info()->pass->name() == RMU_PASS_NAME);
  TRACE(RMU, 2, "RMU: output unreachable symbols %d",
        output_unreachable_symbols);
  TRACE(RMU, 2, "RMU: remove_no_argument_constructors %d",
        m_remove_no_argument_constructors);
  int num_ignore_check_strings = 0;
  reachability::ReachableAspects reachable_aspects;
  auto reachables = this->compute_reachable_objects(
      scope, *method_override_graph, pm, &num_ignore_check_strings,
      &reachable_aspects, emit_graph_this_run, m_relaxed_keep_class_members,
      m_prune_unreferenced_interfaces, m_prune_uninstantiable_insns,
      m_prune_uncallable_instance_method_bodies, m_throw_propagation,
      m_remove_no_argument_constructors);
  reachability::report(pm, *reachables, reachable_aspects);

  ConcurrentReferencesMap references;
  if (output_unreachable_symbols && m_emit_removed_symbols_references) {
    // Before actually cleaning things up, keep track, if requested, of
    // references of removed symbols (which, of course, will be from dead code).
    gather_references_from_removed_symbols(stores, *reachables, references);
  }

  reanimate_zombie_methods(reachable_aspects);

  auto abstracted_classes = reachability::mark_classes_abstract(
      stores, *reachables, reachable_aspects);
  pm.incr_metric("abstracted_classes", abstracted_classes.size());
  if (sweep_code) {
    remove_uninstantiables_impl::Stats remove_uninstantiables_stats;
    std::atomic<size_t> throws_inserted{0};
    InsertOnlyConcurrentSet<DexMethod*> affected_methods;
    reachability::sweep_code(stores, m_prune_uncallable_instance_method_bodies,
                             m_prune_uncallable_virtual_methods,
                             reachable_aspects, &remove_uninstantiables_stats,
                             &throws_inserted, &affected_methods);
    remove_uninstantiables_stats.report(pm);
    pm.incr_metric("throws_inserted", (size_t)throws_inserted);
    pm.incr_metric("methods_with_code_changes", affected_methods.size());
    UnorderedSet<DexMethodRef*> pure_methods;
    LocalDce::Stats dce_stats;
    std::mutex dce_stats_mutex;
    workqueue_run<DexMethod*>(
        [&](DexMethod* method) {
          LocalDce dce(init_classes_with_side_effects.get(), pure_methods);
          dce.dce(method->get_code()->cfg(), /* normalize_new_instances */ true,
                  method->get_class());
          auto local_stats = dce.get_stats();
          std::lock_guard<std::mutex> lock(dce_stats_mutex);
          dce_stats += local_stats;
        },
        affected_methods);
    pm.incr_metric("instructions_eliminated_localdce_dead",
                   dce_stats.dead_instruction_count);
    pm.incr_metric("instructions_eliminated_localdce_unreachable",
                   dce_stats.unreachable_instruction_count);
  }
  reachability::sweep(stores, *reachables,
                      output_unreachable_symbols ? &removed_symbols : nullptr,
                      m_output_full_removed_symbols);
  if (m_prune_uncallable_virtual_methods) {
    auto uninstantiables_stats = reachability::sweep_uncallable_virtual_methods(
        stores, reachable_aspects);
    uninstantiables_stats.report(pm);
    {}
  }

  reachability::ObjectCounts after = reachability::count_objects(stores);
  TRACE(RMU, 1, "after: %zu classes, %zu fields, %zu methods",
        after.num_classes, after.num_fields, after.num_methods);
  pm.incr_metric("num_ignore_check_strings", num_ignore_check_strings);
  pm.incr_metric("classes_removed", before.num_classes - after.num_classes);
  pm.incr_metric("fields_removed", before.num_fields - after.num_fields);
  pm.incr_metric("methods_removed", before.num_methods - after.num_methods);

  if (output_unreachable_symbols) {
    std::string filepath = conf.metafile(UNREACHABLE_SYMBOLS_FILENAME);
    write_out_removed_symbols(filepath, removed_symbols);

    if (m_emit_removed_symbols_references) {
      std::string references_filepath =
          conf.metafile(REMOVED_SYMBOLS_REFERENCES_FILENAME);
      write_out_removed_symbols_references(references_filepath, removed_symbols,
                                           references);
    }
  }
  if (emit_graph_this_run) {
    {
      Timer t("Writing reachability graph");
      std::ofstream os;
      open_or_die(conf.metafile("reachability-graph"), &os);
      reachability::dump_graph(os, reachables->retainers_of());
    }
    {
      Timer t("Writing method-override graph");
      std::ofstream os;
      open_or_die(conf.metafile("method-override-graph"), &os);
      method_override_graph = mog::build_graph(build_class_scope(stores));
      method_override_graph->dump(os);
    }
  }
}

void RemoveUnreachablePassBase::write_out_removed_symbols(
    const std::string& filepath,
    const ConcurrentSet<std::string>& removed_symbols) {
  std::fstream out(filepath, std::ios_base::app);
  if (!out.is_open()) {
    fprintf(stderr, "Unable to write the removed symbols into file %s\n",
            filepath.c_str());
    return;
  }
  TRACE(RMU, 4, "Writing %zu removed symbols to %s", removed_symbols.size(),
        filepath.c_str());
  struct StringPtrComparator {
    bool operator()(const std::string* s1, const std::string* s2) const {
      return *s1 < *s2;
    }
  };
  std::set<const std::string*, StringPtrComparator> sorted;
  unordered_transform(removed_symbols,
                      std::inserter(sorted, sorted.end()),
                      [](const std::string& s) { return &s; });
  for (auto s_ptr : sorted) {
    out << *s_ptr << std::endl;
  }
}

std::unique_ptr<reachability::ReachableObjects>
RemoveUnreachablePass::compute_reachable_objects(
    const Scope& scope,
    const method_override_graph::Graph& method_override_graph,
    PassManager& /* pm */,
    int* num_ignore_check_strings,
    reachability::ReachableAspects* reachable_aspects,
    bool emit_graph_this_run,
    bool relaxed_keep_class_members,
    bool relaxed_keep_interfaces,
    bool cfg_gathering_check_instantiable,
    bool cfg_gathering_check_instance_callable,
    bool cfg_gathering_check_returning,
    bool remove_no_argument_constructors) {
  return reachability::compute_reachable_objects(
      scope, method_override_graph, m_ignore_sets, num_ignore_check_strings,
      reachable_aspects, emit_graph_this_run, relaxed_keep_class_members,
      relaxed_keep_interfaces, cfg_gathering_check_instantiable,
      cfg_gathering_check_instance_callable, cfg_gathering_check_returning,
      false, remove_no_argument_constructors);
}

static RemoveUnreachablePass s_pass;
