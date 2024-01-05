/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexLimitsChecker.h"

#include <sstream>

#include "Debug.h"
#include "DexClass.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"

namespace redex_properties {

void DexLimitsChecker::run_checker(DexStoresVector& stores,
                                   ConfigFiles& /* conf */,
                                   PassManager& mgr,
                                   bool established) {
  // Temporary work around.
  return;
  if (!established) {
    return;
  }
  Timer t("ref_validation");
  std::string pass_name = "initial state";
  auto info = mgr.get_current_pass_info();
  if (info != nullptr) {
    pass_name = info->name;
  }

  std::ostringstream result;

  auto check_ref_num = [&pass_name, &result](const DexClasses& classes,
                                             const DexStore& store,
                                             size_t dex_id) {
    constexpr size_t limit = 65536;
    std::unordered_set<DexMethodRef*> total_method_refs;
    std::unordered_set<DexFieldRef*> total_field_refs;
    std::unordered_set<DexType*> total_type_refs;
    for (const auto cls : classes) {
      std::vector<DexMethodRef*> method_refs;
      std::vector<DexFieldRef*> field_refs;
      std::vector<DexType*> type_refs;
      cls->gather_methods(method_refs);
      cls->gather_fields(field_refs);
      cls->gather_types(type_refs);
      total_type_refs.insert(type_refs.begin(), type_refs.end());
      total_field_refs.insert(field_refs.begin(), field_refs.end());
      total_method_refs.insert(method_refs.begin(), method_refs.end());
    }
    TRACE(PM, 2, "dex %s: method refs %zu, field refs %zu, type refs %zu",
          dex_name(store, dex_id).c_str(), total_method_refs.size(),
          total_field_refs.size(), total_type_refs.size());
    auto check = [&](auto& c, const char* type) {
      if (c.size() > limit) {
        result << pass_name << " adds too many " << type << " refs in dex "
               << dex_name(store, dex_id) << "\n";
      }
    };
    check(total_method_refs, "method");
    check(total_field_refs, "field");
    check(total_type_refs, "type");
  };
  for (const auto& store : stores) {
    size_t dex_id = 0;
    for (const auto& classes : store.get_dexen()) {
      check_ref_num(classes, store, dex_id++);
    }
  }
  auto result_str = result.str();
  always_assert_log(result_str.empty(), "%s", result_str.c_str());
}

} // namespace redex_properties

namespace {
static redex_properties::DexLimitsChecker s_checker;
} // namespace
