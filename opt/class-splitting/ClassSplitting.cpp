/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass splits out static methods with weight 0
 * in the cold-start dexes.
 * The approach here is a new interdex plugin. This enables...
 * - only treating classes that end up in the non-primary cold-start dexes;
 * - accounting for extra classes, which is important to determine when a dex
 *   is full.
 *
 * Relocated methods are moved into new special classes at the end of the dex.
 * Each class is filled with up to a configurable number of methods; only when
 * a class is full, another one is created. Separate classes are created for
 * distinct required api levels.
 */

#include "ClassSplitting.h"

#include <algorithm>
#include <vector>

#include "ApiLevelChecker.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPass.h"
#include "PluginRegistry.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_RELOCATION_CLASSES =
    "num_class_splitting_relocation_classes";
constexpr const char* METRIC_RELOCATED_STATIC_METHODS =
    "num_class_splitting_relocated_static_methods";

struct ClassSplittingStats {
  size_t relocation_classes{0};
  size_t relocated_static_methods{0};
};

class ClassSplittingInterDexPlugin : public interdex::InterDexPassPlugin {
 public:
  ClassSplittingInterDexPlugin(size_t target_class_size_threshold,
                               PassManager& mgr)
      : m_target_class_size_threshold(target_class_size_threshold),
        m_mgr(mgr) {}

  void configure(const Scope& scope, ConfigFiles& cfg) override {
    m_method_to_weight = &cfg.get_method_to_weight();
    m_method_sorting_whitelisted_substrings =
        &cfg.get_method_sorting_whitelisted_substrings();
  };

  bool should_skip_class(const DexClass* clazz) override { return false; }

  void gather_refs(const interdex::DexInfo& dex_info,
                   const DexClass* cls,
                   std::vector<DexMethodRef*>& mrefs,
                   std::vector<DexFieldRef*>& frefs,
                   std::vector<DexType*>& trefs,
                   std::vector<DexClass*>* erased_classes,
                   bool should_not_relocate_methods_of_class) override {
    // Here, we are going to check if any methods in the given class should
    // be relocated. If so, we make sure that we account for possibly needed
    // extra target classes.

    // We are only going to relocate perf-critical classes, i.e. classes in
    // cold-start dexes that are not in the primary dex. (Reshuffling methods
    // in the primary dex may cause issues as it may cause references to
    // secondary dexes to be inspected by the VM too early.)
    always_assert(!dex_info.mixed_mode || dex_info.coldstart);
    if (!dex_info.coldstart || dex_info.primary) {
      return;
    }

    // Bail out if we just cannot or should not relocate methods of this class.
    if (!can_relocate(cls) || should_not_relocate_methods_of_class) {
      return;
    }

    always_assert(!cls->rstate.is_generated());

    SplitClass& sc = m_split_classes[cls];
    always_assert(sc.relocatable_methods.size() == 0);
    for (DexMethod* method : cls->get_dmethods()) {
      if (!can_relocate(method)) {
        continue;
      }
      unsigned int weight =
          get_method_weight_if_available(method, m_method_to_weight);
      if (weight == 0) {
        weight = get_method_weight_override(
            method, m_method_sorting_whitelisted_substrings);
      }
      if (weight > 0) {
        continue;
      }
      int api_level = api::LevelChecker::get_method_level(method);
      TargetClassInfo& target_class_info = m_target_classes[api_level];
      if (target_class_info.target_cls == nullptr ||
          (target_class_info.last_source_cls != cls &&
           target_class_info.size >= m_target_class_size_threshold)) {
        std::stringstream ss;
        ss << "Lredex/$Relocated" << std::to_string(m_next_target_class_index++)
           << "ApiLevel" << std::to_string(api_level++) << ";";
        std::string target_type_name(ss.str());
        DexType* target_type = DexType::make_type(target_type_name.c_str());
        ++m_stats.relocation_classes;
        ClassCreator cc(target_type);
        cc.set_access(ACC_PUBLIC | ACC_FINAL);
        cc.set_super(get_object_type());
        DexClass* target_cls = cc.create();
        target_cls->rstate.set_generated();
        target_class_info.target_cls = target_cls;
        target_class_info.last_source_cls = cls;
        target_class_info.size = 0;
      }
      ++target_class_info.size;
      sc.relocatable_methods.insert(
          {method, {target_class_info.target_cls, api_level}});
      trefs.push_back(target_class_info.target_cls->get_type());
      TRACE(CS, 4,
            "[class splitting] Method {%s} with weight %d will be relocated to "
            "{%s}\n",
            SHOW(method), weight, SHOW(target_class_info.target_cls));
    }
  }

  DexClasses additional_classes(const DexClassesVector& outdex,
                                const DexClasses& classes) override {
    // Here, we are going to do the final determination of what to relocate ---
    // After checking if things still look as they did before, and no other
    // interdex pass or feature tinkered with the relocatability...
    // The actual relocation will happen in cleanup, so that we don't interfere
    // with earlier InterDex cleanups that still expect the code to be in their
    // original places.

    DexClasses target_classes;
    std::unordered_set<const DexClass*> target_classes_set;
    size_t relocated_methods = 0;
    // We iterate over the actually added set of classes.
    for (DexClass* cls : classes) {
      auto split_classes_it = m_split_classes.find(cls);
      if (split_classes_it == m_split_classes.end()) {
        continue;
      }
      const SplitClass& sc = split_classes_it->second;
      if (sc.relocatable_methods.size() == 0) {
        continue;
      }
      if (!can_relocate(cls)) {
        TRACE(CS,
              4,
              "[class splitting] Class earlier identified as relocatable is "
              "no longer relocatable: {%s}\n",
              SHOW(cls));
        continue;
      }
      std::vector<DexMethod*> methods_to_relocate;
      // We iterate over the actually existing set of methods at this time
      // (other InterDex plug-ins might have added or removed or relocated
      // methods).
      for (DexMethod* method : cls->get_dmethods()) {
        auto it = sc.relocatable_methods.find(method);
        if (it == sc.relocatable_methods.end()) {
          continue;
        }
        const RelocatableMethodInfo& method_info = it->second;
        if (!can_relocate(method)) {
          TRACE(CS,
                4,
                "[class splitting] Method earlier identified as relocatable is "
                "not longer relocatable: {%s}\n",
                SHOW(method));
          continue;
        }
        int api_level = api::LevelChecker::get_method_level(method);
        if (api_level != method_info.api_level) {
          TRACE(CS, 4,
                "[class splitting] Method {%s} api level changed to {%d} from "
                "{%d}.\n",
                SHOW(method), api_level, method_info.api_level);
          continue;
        }

        methods_to_relocate.push_back(method);
      }

      for (DexMethod* method : methods_to_relocate) {
        const RelocatableMethodInfo& method_info =
            sc.relocatable_methods.at(method);

        m_methods_to_relocate.emplace_back(method, method_info.target_cls);
        ++relocated_methods;
        if (is_static(method)) {
          ++m_stats.relocated_static_methods;
        }

        TRACE(CS, 3, "[class splitting] Method {%s} relocated to {%s}\n",
              SHOW(method), SHOW(method_info.target_cls));

        if (target_classes_set.insert(method_info.target_cls).second) {
          target_classes.push_back(method_info.target_cls);
        }
      }
    }

    TRACE(CS, 2,
          "[class splitting] Relocated {%zu} methods to {%zu} target classes "
          "in this dex.\n",
          relocated_methods, target_classes.size());

    m_target_classes.clear();
    m_split_classes.clear();
    return target_classes;
  }

  void cleanup(const std::vector<DexClass*>& scope) override {
    // Here we do the actual relocation.
    for (auto& p : m_methods_to_relocate) {
      DexMethod* method = p.first;
      DexClass* target_cls = p.second;
      set_public(method);
      relocate_method(method, target_cls->get_type());
      change_visibility(method);
    }

    m_mgr.incr_metric(METRIC_RELOCATION_CLASSES, m_stats.relocation_classes);
    m_mgr.incr_metric(METRIC_RELOCATED_STATIC_METHODS,
                      m_stats.relocated_static_methods);

    // Releasing memory
    m_target_classes.clear();
    m_split_classes.clear();
    m_methods_to_relocate.clear();
  }

 private:
  const std::unordered_map<std::string, unsigned int>* m_method_to_weight{
      nullptr};

  const std::unordered_set<std::string>*
      m_method_sorting_whitelisted_substrings{nullptr};

  struct RelocatableMethodInfo {
    DexClass* target_cls;
    int32_t api_level;
  };

  struct SplitClass {
    std::unordered_map<DexMethod*, RelocatableMethodInfo> relocatable_methods;
  };

  struct TargetClassInfo {
    DexClass* target_cls{nullptr};
    const DexClass* last_source_cls{nullptr};
    size_t size{0}; // number of methods
  };

  std::unordered_map<int32_t, TargetClassInfo> m_target_classes;
  size_t m_next_target_class_index{0};
  size_t m_target_class_size_threshold;
  std::unordered_map<const DexClass*, SplitClass> m_split_classes;
  std::vector<std::pair<DexMethod*, DexClass*>> m_methods_to_relocate;
  ClassSplittingStats m_stats;

  bool can_relocate(const DexClass* cls) {
    return !cls->get_clinit() && !cls->is_external() &&
           !cls->rstate.is_generated();
  }

  bool can_relocate(const DexMethod* m) {
    if (!m->is_concrete() || m->is_external() || !m->get_code() ||
        !can_rename(m) || root(m) || m->rstate.no_optimizations() ||
        !gather_invoked_direct_methods_that_prevent_relocation(m) ||
        !no_invoke_super(m) || m->rstate.is_generated()) {
      return false;
    }
    return is_static(m) && !is_clinit(m);
  };

  PassManager& m_mgr;
};

} // namespace

void ClassSplittingPass::configure_pass(const JsonWrapper& jw) {
  jw.get("relocated_methods_per_target_class", 64,
         m_relocated_methods_per_target_class);
}

void ClassSplittingPass::run_pass(DexStoresVector&,
                                  ConfigFiles&,
                                  PassManager& mgr) {
  interdex::InterDexRegistry* registry =
      static_cast<interdex::InterDexRegistry*>(
          PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
  std::function<interdex::InterDexPassPlugin*()> fn =
      [this, &mgr]() -> interdex::InterDexPassPlugin* {
    return new ClassSplittingInterDexPlugin(
        m_relocated_methods_per_target_class, mgr);
  };
  registry->register_plugin("CLASS_SPLITTING_PLUGIN", std::move(fn));
}

static ClassSplittingPass s_pass;
