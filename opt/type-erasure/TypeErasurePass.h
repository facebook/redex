/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <boost/optional.hpp>
#include <unordered_set>

struct ModelSpec;
class ModelMerger;

class TypeErasurePass : public Pass {
 public:
  TypeErasurePass() : Pass("TypeErasurePass") {}
  explicit TypeErasurePass(const char* name) : Pass(name) {}

  void configure_pass(const JsonWrapper& jw) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 protected:
  virtual ModelMerger* get_model_merger();

 private:
  std::string m_merged_type_mapping_file;
  std::vector<ModelSpec> m_model_specs;
  std::vector<ModelSpec> m_dex_sharding_model_specs;
  boost::optional<size_t> m_max_num_dispatch_target = boost::none;

 private:
  void erase_model(const ModelSpec& spec,
                   Scope& scope,
                   PassManager& mgr,
                   DexStoresVector& stores,
                   ConfigFiles& cfg);
};
