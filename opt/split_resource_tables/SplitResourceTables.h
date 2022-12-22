/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

using SwitchIndices = std::set<int>;

class SplitResourceTablesPass : public Pass {
 public:
  SplitResourceTablesPass() : Pass("SplitResourceTablesPass") {}

  void bind_config() override {
    bind("allowed_types", {}, m_allowed_types);
    bind("static_ids", "", m_static_ids_file_path);
    bind("getidentifier_compat_method", "", m_getidentifier_compat_method);
    bind("typename_compat_method", "", m_typename_compat_method);
    // TODO(T44504426) coercer should assert the non-negativity of the parsed
    // values
    bind("split_threshold", 50, m_split_threshold);
    bind("max_splits_per_type", 5, m_max_splits_per_type);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool is_type_allowed(const std::string& type_name);
  std::unordered_set<std::string> m_allowed_types;
  std::string m_static_ids_file_path;
  std::string m_getidentifier_compat_method;
  std::string m_typename_compat_method;
  size_t m_split_threshold;
  size_t m_max_splits_per_type;
};
