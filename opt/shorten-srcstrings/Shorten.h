/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ShortenSrcStringsPass : public Pass {
 public:
  ShortenSrcStringsPass() : Pass("ShortenSrcStringsPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("filename_mappings", "filename_mappings.txt", m_filename_mappings);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_filename_mappings;
};
