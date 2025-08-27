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
  bool m_include_strings_lookup_class;
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
  std::optional<bool> m_override_strip_classes;
};
