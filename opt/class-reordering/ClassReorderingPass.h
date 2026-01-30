/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "Pass.h"

class ClassReorderingPass : public Pass {
 public:
  ClassReorderingPass() : Pass("ClassReorderingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  std::string get_config_doc() override {
    return "When enabled, this pass will reorder the classes intradex to meet "
           "dex37 verifier requirements that all classes' interface and super "
           "class, if present in the same dex, must appear before them in "
           "class definitions.";
  }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
