/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Pass.h"
#include "StringSwitchTransform.h"

class StringSwitchTransformPass : public Pass {
 public:
  StringSwitchTransformPass() : Pass("StringSwitchTransformPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {{DexLimitsObeyed, Preserves},
            {UltralightCodePatterns, Preserves},
            {NoInitClassInstructions, Preserves},
            {RenameClass, Preserves}};
  }

  std::string get_config_doc() override {
    return "Recognizes switches over java/lang/String objects and, when a "
           "transform is configured, rewrites them into a more efficient form.";
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  // Builds the prioritized list of transforms enabled by this pass's config.
  // May be empty (then the pass only performs analysis).
  std::vector<std::unique_ptr<StringSwitchTransform>> build_transforms() const;

  // When true, the per-switch analysis report is written to a metafile.
  bool m_emit_analysis{true};
  // Min number of cases (incl. default) for the StringTreeMap transform.
  int64_t m_min_cases{26};
  // Fully-qualified static lookup method for the StringTreeMap transform; empty
  // disables that transform.
  std::string m_string_tree_lookup_method;
};
