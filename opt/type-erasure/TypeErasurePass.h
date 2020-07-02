/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <boost/optional.hpp>
#include <unordered_set>

#include "Model.h"

class ModelMerger;

class TypeErasurePass : public Pass {
 public:
  TypeErasurePass() : Pass("TypeErasurePass") {}
  explicit TypeErasurePass(const char* name) : Pass(name) {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_merged_type_mapping_file;
  std::vector<ModelSpec> m_model_specs;
  boost::optional<size_t> m_max_num_dispatch_target = boost::none;
};
