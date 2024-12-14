/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ModelSpecGenerator.h"

#include "ClassMerging.h"
#include "LiveRange.h"
#include "Model.h"
#include "PassManager.h"
#include "ReflectionAnalysis.h"
#include "Show.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {

/**
 * The methods and fields may have associated keeping rules, exclude the classes
 * if they or their methods/fields are not deleteable. For example, methods
 * annotated with @android.webkit.JavascriptInterface are invoked reflectively,
 * and we should keep them according to their keeping rules.
 *
 * In practice, we find some constructors of anonymous classes are kept by
 * overly-conservative rules. So we loose the checking for the constructors of
 * anonymous classes.
 */
bool can_delete_class(const DexClass* cls, bool is_anonymous_class) {
  if (!can_delete(cls)) {
    return false;
  }
  auto& vmethods = cls->get_vmethods();
  if (std::any_of(vmethods.begin(), vmethods.end(),
                  [](const DexMethod* m) { return !can_delete(m); })) {
    return false;
  }
  auto& dmethods = cls->get_dmethods();
  if (std::any_of(dmethods.begin(), dmethods.end(),
                  [&is_anonymous_class](const DexMethod* m) {
                    return (!is_anonymous_class || !is_constructor(m)) &&
                           !can_delete(m);
                  })) {
    return false;
  }
  auto& ifields = cls->get_ifields();
  if (std::any_of(ifields.begin(), ifields.end(),
                  [](const DexField* f) { return !can_delete(f); })) {
    return false;
  }
  auto& sfields = cls->get_sfields();
  if (std::any_of(sfields.begin(), sfields.end(),
                  [](const DexField* f) { return !can_delete(f); })) {
    return false;
  }
  return true;
}

TypeSet collect_reflected_mergeables(
    reflection::MetadataCache& refl_metadata_cache,
    class_merging::ModelSpec* merging_spec,
    DexMethod* method) {
  TypeSet non_mergeables;
  auto code = method->get_code();
  if (!code) {
    return non_mergeables;
  }
  std::unique_ptr<reflection::ReflectionAnalysis> analysis =
      std::make_unique<reflection::ReflectionAnalysis>(
          /* dex_method */ method,
          /* context (interprocedural only) */ nullptr,
          /* summary_query_fn (interprocedural only) */ nullptr,
          /* metadata_cache */ &refl_metadata_cache);

  if (!analysis->has_found_reflection()) {
    return non_mergeables;
  }

  auto& cfg = code->cfg();
  live_range::MoveAwareChains chains(cfg);
  live_range::DefUseChains du_chains = chains.get_def_use_chains();

  for (const auto& mie : cfg::InstructionIterable(cfg)) {
    auto insn = mie.insn;
    auto aobj = analysis->get_result_abstract_object(insn);

    DexType* reflected_type = nullptr;
    if (aobj && aobj->is_class() && aobj->get_dex_type()) {
      reflected_type = const_cast<DexType*>(
          type::get_element_type_if_array(aobj->get_dex_type()));
    }
    if (reflected_type == nullptr ||
        merging_spec->merging_targets.count(reflected_type) == 0) {
      continue;
    }
    const auto use_set = du_chains[insn];
    if (merging_spec->mergeability_checks_use_of_const_class &&
        use_set.empty()) {
      TRACE(CLMG, 5, "[reflected mergeable] skipped w/o no use %s in %s",
            SHOW(insn), SHOW(method));
      continue;
    }

    non_mergeables.insert(reflected_type);
    TRACE(CLMG, 5, "[reflected mergeable] %s (%s) in %s", SHOW(insn),
          SHOW(reflected_type), SHOW(method));
  }

  return non_mergeables;
}

void drop_reflected_mergeables(const Scope& scope,
                               class_merging::ModelSpec* merging_spec) {
  reflection::MetadataCache refl_metadata_cache;
  TypeSet reflected_mergeables =
      walk::parallel::methods<TypeSet, MergeContainers<TypeSet>>(
          scope, [&](DexMethod* meth) {
            return collect_reflected_mergeables(refl_metadata_cache,
                                                merging_spec, meth);
          });

  for (const auto* type : reflected_mergeables) {
    merging_spec->merging_targets.erase(type);
  }
}

} // namespace

namespace class_merging {

void find_all_mergeables_and_roots(const TypeSystem& type_system,
                                   const Scope& scope,
                                   size_t global_min_count,
                                   PassManager& mgr,
                                   ModelSpec* merging_spec,
                                   bool skip_dynamically_dead) {
  std::unordered_map<const DexTypeList*, std::vector<const DexType*>>
      intfs_implementors;
  std::unordered_map<const DexType*, std::vector<const DexType*>>
      parent_children;
  TypeSet throwable;
  type_system.get_all_children(type::java_lang_Throwable(), throwable);
  for (const auto* cls : scope) {
    auto cur_type = cls->get_type();
    if (is_interface(cls) || is_abstract(cls) || cls->rstate.is_generated() ||
        cls->get_clinit() || throwable.count(cur_type)) {
      continue;
    }
    if (skip_dynamically_dead && cls->is_dynamically_dead()) {
      continue;
    }
    bool is_anonymous_class = klass::maybe_anonymous_class(cls);
    // TODO: Can merge named classes.
    if (!is_anonymous_class) {
      continue;
    }
    TypeSet children;
    type_system.get_all_children(cur_type, children);
    if (!children.empty()) {
      continue;
    }
    if (!can_delete_class(cls, is_anonymous_class)) {
      continue;
    }
    auto* intfs = cls->get_interfaces();
    auto super_cls = cls->get_super_class();
    if (super_cls != type::java_lang_Object()) {
      parent_children[super_cls].push_back(cur_type);
    } else if (!intfs->empty()) {
      intfs_implementors[intfs].push_back(cur_type);
    } else {
      // TODO: Investigate error P444184021 when merging simple classes without
      // interfaces.
    }
  }
  for (const auto& pair : parent_children) {
    auto parent = pair.first;
    if (!type_class(parent)) {
      continue;
    }
    auto& children = pair.second;
    if (children.size() >= global_min_count) {
      TRACE(CLMG,
            9,
            "Discover root %s with %zu child classes",
            SHOW(parent),
            children.size());
      merging_spec->roots.insert(parent);
      merging_spec->merging_targets.insert(children.begin(), children.end());
      mgr.incr_metric("cls_" + show(parent), children.size());
    }
  }
  for (const auto& pair : intfs_implementors) {
    auto intfs = pair.first;
    const auto has_defs = [&intfs]() {
      for (const auto* intf : *intfs) {
        if (!type_class(intf)) {
          return false;
        }
      }
      return true;
    };
    if (!has_defs()) {
      // Skip if any interface definition is missing.
      continue;
    }
    auto& implementors = pair.second;
    if (implementors.size() >= global_min_count) {
      TRACE(CLMG,
            9,
            "Discover interface root %s with %zu implementors",
            SHOW(intfs),
            pair.second.size());
      auto first_implementor = type_class(implementors[0]);
      merging_spec->roots.insert(first_implementor->get_super_class());
      merging_spec->merging_targets.insert(implementors.begin(),
                                           implementors.end());
      mgr.incr_metric("intf_" + show(intfs), implementors.size());
    }
  }

  drop_reflected_mergeables(scope, merging_spec);
  TRACE(CLMG, 9, "Discover %zu mergeables from %zu roots",
        merging_spec->merging_targets.size(), merging_spec->roots.size());
}

class_merging::Model construct_global_model(
    DexClasses& scope,
    PassManager& mgr,
    ConfigFiles& conf,
    DexStoresVector& stores,
    const class_merging::ModelSpec& merging_spec,
    size_t global_min_count) {
  // Copy merging_spec to avoid changing the original one.
  class_merging::ModelSpec global_model_merging_spec = merging_spec;
  // The global_model_merging_spec share everything with the input merging_spec
  // expect for the following, which removes dex boundaries and max size of
  // mergers to create a global model.
  global_model_merging_spec.per_dex_grouping = false;
  global_model_merging_spec.strategy = class_merging::strategy::BY_CLASS_COUNT;
  global_model_merging_spec.min_count = 2;
  global_model_merging_spec.max_count = std::numeric_limits<size_t>::max();

  TypeSystem type_system(scope);
  find_all_mergeables_and_roots(type_system, scope,
                                /*global_min_count=*/global_min_count, mgr,
                                &global_model_merging_spec);
  return class_merging::construct_model(type_system, scope, conf, mgr, stores,
                                        global_model_merging_spec);
};

} // namespace class_merging
