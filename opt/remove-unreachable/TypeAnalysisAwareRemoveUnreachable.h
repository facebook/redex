/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalTypeAnalysisPass.h"
#include "Pass.h"
#include "Reachability.h"

class TypeAnalysisAwareRemoveUnreachablePass : public Pass {
 public:
  TypeAnalysisAwareRemoveUnreachablePass()
      : Pass("TypeAnalysisAwareRemoveUnreachablePass") {}

  void bind_config() override {
    bind("ignore_string_literals", {}, m_ignore_sets.string_literals);
    bind("ignore_string_literal_annos", {}, m_ignore_sets.string_literal_annos);
    bind("ignore_system_annos", {}, m_ignore_sets.system_annos);
    bind("keep_class_in_string", true, m_ignore_sets.keep_class_in_string);
    after_configuration([this] {
      // To keep the backward compatability of this code, ensure that the
      // "MemberClasses" annotation is always in system_annos.
      m_ignore_sets.system_annos.emplace(
          DexType::get_type("Ldalvik/annotation/MemberClasses;"));
    });
  }

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<GlobalTypeAnalysisPass>();
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  reachability::IgnoreSets m_ignore_sets;
};
