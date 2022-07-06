/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <boost/optional.hpp>
#include <unordered_set>

#include "Model.h"

namespace class_merging {

class ModelMerger;

class ClassMergingPass : public Pass {
 public:
  ClassMergingPass() : Pass("ClassMergingPass") {}
  explicit ClassMergingPass(const char* name) : Pass(name) {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_merged_type_mapping_file;
  std::vector<ModelSpec> m_model_specs;
  boost::optional<size_t> m_max_num_dispatch_target = boost::none;
};

} // namespace class_merging
