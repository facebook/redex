/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ModelSpecGenerator.h"
#include "Model.h"
#include "PassManager.h"
#include "ReflectionAnalysis.h"
#include "Show.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {

/**
 * Return true if the name matches "$$Lambda$", "$$ExternalSyntheticLambda", or
 * "$[0-9]".
 */
bool maybe_anonymous_class(const DexClass* cls) {
  static constexpr std::array<const char*, 2> patterns = {
      // https://r8.googlesource.com/r8/+/refs/tags/3.1.34/src/main/java/com/android/tools/r8/synthesis/SyntheticNaming.java#140
      "$$ExternalSyntheticLambda",
      // Desugared lambda classes from older versions of D8.
      "$$Lambda$",
  };
  const auto name = cls->get_deobfuscated_name_or_empty();
  auto pos = name.rfind('$');
  if (pos == std::string::npos) {
    return false;
  }
  pos++;
  return (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ||
         std::any_of(patterns.begin(), patterns.end(),
                     [&name](const std::string& pattern) {
                       return name.find(pattern) != std::string::npos;
                     });
}

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
  for (const auto& mie : cfg::InstructionIterable(cfg)) {
    auto insn = mie.insn;
    auto aobj = analysis->get_result_abstract_object(insn);

    DexType* reflected_type = nullptr;
    if (aobj && aobj->is_class() && aobj->get_dex_type()) {
      reflected_type = const_cast<DexType*>(
          type::get_element_type_if_array(aobj->get_dex_type()));
    }
    if (reflected_type != nullptr &&
        merging_spec->merging_targets.count(reflected_type) > 0) {
      non_mergeables.insert(reflected_type);
      TRACE(CLMG, 5, "[reflected mergeable] %s (%s) in %s", SHOW(insn),
            SHOW(reflected_type), SHOW(method));
    }
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
                                   ModelSpec* merging_spec) {
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
    bool is_anonymous_class = maybe_anonymous_class(cls);
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
    auto intf = pair.first;
    auto& implementors = pair.second;
    if (implementors.size() >= global_min_count) {
      TRACE(CLMG,
            9,
            "Discover interface root %s with %zu implementors",
            SHOW(intf),
            pair.second.size());
      auto first_implementor = type_class(implementors[0]);
      merging_spec->roots.insert(first_implementor->get_super_class());
      merging_spec->merging_targets.insert(implementors.begin(),
                                           implementors.end());
      mgr.incr_metric("intf_" + show(intf), implementors.size());
    }
  }

  drop_reflected_mergeables(scope, merging_spec);
  TRACE(CLMG, 9, "Discover %zu mergeables from %zu roots",
        merging_spec->merging_targets.size(), merging_spec->roots.size());
}

} // namespace class_merging
