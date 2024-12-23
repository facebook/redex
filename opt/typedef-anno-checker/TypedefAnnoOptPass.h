/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "Pass.h"
#include "TypeInference.h"

/*
 * This pass uses StringTreeMap to mimic the valueOf enum capability in a more
 * efficient way. It then replaces all calls to the default valueOf method with
 * calls to the alternative valueOfOpt method.
 */
class TypedefAnnoOptPass : public Pass {
 public:
  TypedefAnnoOptPass() : Pass("TypedefAnnoOptPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    return redex_properties::simple::preserves_all();
  }

  struct Config {
    DexType* int_typedef{nullptr};
    DexType* str_typedef{nullptr};
  };

  void bind_config() override {
    bind("int_typedef", {}, m_config.int_typedef);
    bind("str_typedef", {}, m_config.str_typedef);
  }

  explicit TypedefAnnoOptPass(Config config)
      : Pass("TypedefAnnoOptPass"), m_config(config) {}

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  void populate_value_of_opt_str(DexClass* cls);

  friend struct TypedefAnnoOptTest;

  Config m_config;
  std::unordered_map<DexMethod*, DexMethod*> old_to_new_callee;
};
