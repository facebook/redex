/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass sorts non-perf sensitive classes according to their inheritance
 * hierarchies in each dex. This improves compressibility.
 */
#include "SortRemainingClassesPass.h"

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "InterDex.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
// Compare two classes for sorting in a way that is best for compression.
bool compare_dexclasses_for_compressed_size(DexClass* c1, DexClass* c2) {
  // Canary classes go first
  if (is_canary(c1) != is_canary(c2)) {
    return (is_canary(c1) ? 1 : 0) > (is_canary(c2) ? 1 : 0);
  }
  // Interfaces go after non-interfaces
  if (is_interface(c1) != is_interface(c2)) {
    return (is_interface(c1) ? 1 : 0) < (is_interface(c2) ? 1 : 0);
  }
  // Base types and implemented interfaces go last
  if (type::check_cast(c2->get_type(), c1->get_type())) {
    return false;
  }
  always_assert(c1 != c2);
  if (type::check_cast(c1->get_type(), c2->get_type())) {
    return true;
  }
  // If types are unrelated, sort by super-classes and then
  // interfaces
  if (c1->get_super_class() != c2->get_super_class()) {
    return compare_dextypes(c1->get_super_class(), c2->get_super_class());
  }
  if (c1->get_interfaces() != c2->get_interfaces()) {
    return compare_dextypelists(c1->get_interfaces(), c2->get_interfaces());
  }

  // Tie-breaker: fields/methods count distance
  int dmethods_distance =
      (int)c1->get_dmethods().size() - (int)c2->get_dmethods().size();
  if (dmethods_distance != 0) {
    return dmethods_distance < 0;
  }
  int vmethods_distance =
      (int)c1->get_vmethods().size() - (int)c2->get_vmethods().size();
  if (vmethods_distance != 0) {
    return vmethods_distance < 0;
  }
  int ifields_distance =
      (int)c1->get_ifields().size() - (int)c2->get_ifields().size();
  if (ifields_distance != 0) {
    return ifields_distance < 0;
  }
  int sfields_distance =
      (int)c1->get_sfields().size() - (int)c2->get_sfields().size();
  if (sfields_distance != 0) {
    return sfields_distance < 0;
  }
  // Tie-breaker: has-class-data
  if (c1->has_class_data() != c2->has_class_data()) {
    return (c1->has_class_data() ? 1 : 0) < (c2->has_class_data() ? 1 : 0);
  }
  // Final tie-breaker: Compare types, which means names
  return compare_dextypes(c1->get_type(), c2->get_type());
}

void sort_classes_for_compressed_size(const std::string& name,
                                      const ConfigFiles& conf,
                                      PassManager& mgr,
                                      DexClasses* classes) {
  std::vector<DexClass*> perf_sensitive_classes;
  using DexClassWithSortNum = std::pair<DexClass*, double>;
  std::vector<DexClassWithSortNum> classes_with_sort_num;
  std::vector<DexClass*> remaining_classes;
  using namespace method_profiles;
  // Copy intended!
  auto mpoc =
      *conf.get_global_config().get_config_by_name<MethodProfileOrderingConfig>(
          "method_profile_order");
  mpoc.min_appear_percent = 1.0f;
  dexmethods_profiled_comparator comparator({}, &conf.get_method_profiles(),
                                            &mpoc);
  for (auto cls : *classes) {
    if (cls->is_perf_sensitive() || is_canary(cls)) {
      perf_sensitive_classes.push_back(cls);
      continue;
    }
    double cls_sort_num = dexmethods_profiled_comparator::VERY_END;
    walk::methods(std::vector<DexClass*>{cls}, [&](DexMethod* method) {
      auto method_sort_num = comparator.get_overall_method_sort_num(method);
      if (method_sort_num < cls_sort_num) {
        cls_sort_num = method_sort_num;
      }
    });
    if (cls_sort_num < dexmethods_profiled_comparator::VERY_END) {
      classes_with_sort_num.emplace_back(cls, cls_sort_num);
      continue;
    }
    remaining_classes.push_back(cls);
  }
  always_assert(perf_sensitive_classes.size() + classes_with_sort_num.size() +
                    remaining_classes.size() ==
                classes->size());

  TRACE(SRC_PASS, 3,
        "Skipping %zu perf sensitive, ordering %zu by method profiles, and "
        "sorting %zu classes",
        perf_sensitive_classes.size(), classes_with_sort_num.size(),
        remaining_classes.size());
  std::stable_sort(
      classes_with_sort_num.begin(), classes_with_sort_num.end(),
      [](const DexClassWithSortNum& a, const DexClassWithSortNum& b) {
        return a.second < b.second;
      });
  std::sort(remaining_classes.begin(), remaining_classes.end(),
            compare_dexclasses_for_compressed_size);
  // Rearrange classes so that...
  // - perf_sensitive_classes go first, then
  // - classes_with_sort_num that got ordered by the method profiles, and
  // finally
  // - remaining_classes
  classes->clear();
  classes->insert(classes->end(), perf_sensitive_classes.begin(),
                  perf_sensitive_classes.end());
  for (auto& p : classes_with_sort_num) {
    classes->push_back(p.first);
  }
  classes->insert(classes->end(), remaining_classes.begin(),
                  remaining_classes.end());
  mgr.set_metric(name + "_perf_sensitive_classes",
                 perf_sensitive_classes.size());
  mgr.set_metric(name + "_classes_with_sort_num", classes_with_sort_num.size());
  mgr.set_metric(name + "_remaining_classes", remaining_classes.size());
}

} // namespace

void SortRemainingClassesPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  if (!m_enable_pass) {
    TRACE(SRC_PASS, 1, "SortRemainingClassesPass is disabled.\n");
    return;
  }

  std::vector<std::pair<std::string, DexClasses*>> linear_dexen;
  for (auto& store : stores) {
    auto& dexen = store.get_dexen();
    // by default (m_sort_primary_dex == false), skip primary dex in root store.
    // Otherwise, also sort primary dex.
    for (size_t i = (store.is_root_store() && !m_sort_primary_dex) ? 1 : 0;
         i < dexen.size();
         i++) {
      std::string name = store.get_name();
      if (i > 0) {
        name += std::to_string(i);
      }
      linear_dexen.emplace_back(std::move(name), &dexen.at(i));
    }
  }

  workqueue_run_for<size_t>(0, linear_dexen.size(), [&](size_t i) {
    auto&& [name, dex] = linear_dexen.at(i);
    sort_classes_for_compressed_size(name, conf, mgr, dex);
  });
}

static SortRemainingClassesPass s_pass;
