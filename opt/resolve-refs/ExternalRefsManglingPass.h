/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include "ApiLevelsUtils.h"

/**
 * Base type of a Pass that refines references like method refs to external
 * ones. The abstract pass encodes common logic and states that initializes a
 * min_sdk API if necessary. Having this logic in one place also ensures that
 * all passes that touch external references operate in the same way.
 */
class ExternalRefsManglingPass : public Pass {
 public:
  explicit ExternalRefsManglingPass(const std::string& name) : Pass(name) {}

  void bind_config() override {
    bind("refine_to_external", true, m_refine_to_external,
         "Allowing resolving method ref to an external one");
    bind("supported_min_sdk_for_external_refs", 14,
         m_supported_min_sdk_for_external_refs,
         "If refine_to_external is turned on, the minimal sdk level that can "
         "be supported.");
    bind("excluded_externals", {}, m_excluded_externals,
         "Externals types/prefixes excluded from reference resolution");
  }

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override = 0;

 protected:
  bool m_refine_to_external = true;
  int32_t m_supported_min_sdk_for_external_refs;
  std::vector<std::string> m_excluded_externals;
  const api::AndroidSDK* m_min_sdk_api;
};
