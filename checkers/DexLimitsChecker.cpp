/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexLimitsChecker.h"

#include <sstream>

#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLimitsInfo.h"
#include "DexUtil.h"
#include "InitClassesWithSideEffects.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"

namespace redex_properties {

void DexLimitsChecker::run_checker(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr,
                                   bool established) {
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
  Scope scope = build_class_scope(stores);
  std::unique_ptr<init_classes::InitClassesWithSideEffects>
      init_classes_with_side_effects;
  if (!mgr.init_class_lowering_has_run()) {
    init_classes_with_side_effects =
        std::make_unique<init_classes::InitClassesWithSideEffects>(
            scope, conf.create_init_class_insns());
  }

  auto check_ref_num =
      [&init_classes_with_side_effects, &pass_name, &result](
          const DexClasses& classes, const DexStore& store, size_t dex_id) {
        DexLimitsInfo dex_limits(init_classes_with_side_effects.get());
        for (const auto& cls : classes) {
          if (!dex_limits.update_refs_by_adding_class(cls)) {
            if (dex_limits.is_method_overflow()) {
              result << pass_name << " adds too many method refs in dex "
                     << dex_name(store, dex_id) << "\n";
            }
            if (dex_limits.is_field_overflow()) {
              result << pass_name << " adds too many field refs in dex "
                     << dex_name(store, dex_id) << "\n";
            }
            if (dex_limits.is_type_overflow()) {
              result << pass_name << " adds too many type refs in dex "
                     << dex_name(store, dex_id) << "\n";
            }
          }
        }
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
