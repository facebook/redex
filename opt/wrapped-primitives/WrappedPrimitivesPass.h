/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

// A wrapped primitive is a type with a constructor taking a primitive, that is
// largely used to achieve some special kind of type safety above just a
// primitive. Configurations will specify the wrapper type name, and APIs that
// it is sanctioned to be used in. For wrapper instances that can be replaced
// directly with the primitive itself safely (based on easily understood
// instantiation) this pass will make modifications.
class WrappedPrimitivesPass : public Pass {
 public:
  WrappedPrimitivesPass() : Pass("WrappedPrimitivesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {UltralightCodePatterns, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override;
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  // Used for later validation and informational metrics.
  std::set<std::string> m_wrapper_type_names;
  friend class ValidateWrappedPrimitivesPass;
};

class ValidateWrappedPrimitivesPass : public Pass {
 public:
  ValidateWrappedPrimitivesPass() : Pass("ValidateWrappedPrimitivesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    return redex_properties::simple::preserves_all();
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
