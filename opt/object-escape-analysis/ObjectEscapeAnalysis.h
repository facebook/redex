/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ObjectEscapeAnalysisPass : public Pass {
 public:
  ObjectEscapeAnalysisPass() : Pass("ObjectEscapeAnalysisPass") {}

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_max_inline_size;
};
