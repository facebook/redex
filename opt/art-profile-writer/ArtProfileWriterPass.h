/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class ArtProfileWriterPass : public Pass {
 public:
  ArtProfileWriterPass() : Pass("ArtProfileWriterPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    return redex_properties::simple::preserves_all();
  }

  void bind_config() override;
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_never_inline_estimate;
  bool m_never_inline_attach_annotations;
  int64_t m_never_compile_callcount_threshold;
  int64_t m_never_compile_perf_threshold;
  int64_t m_never_compile_called_coverage_threshold;
  std::string m_never_compile_excluded_interaction_pattern;
  int64_t m_never_compile_excluded_appear100_threshold;
  int64_t m_never_compile_excluded_call_count_threshold;
  bool m_include_strings_lookup_class;
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
};
