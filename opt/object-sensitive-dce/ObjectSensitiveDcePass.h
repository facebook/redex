/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "CallGraph.h"
#include "LocalPointersAnalysis.h"
#include "Pass.h"
#include "SideEffectSummary.h"
#include "UsedVarsAnalysis.h"

class ObjectSensitiveDcePass final : public Pass {
 public:
  ObjectSensitiveDcePass() : Pass("ObjectSensitiveDcePass") {}

  void configure_pass(const JsonWrapper& jw) override {
    std::string s;
    jw.get("side_effect_summaries", "", s);
    if (s != "") {
      m_external_side_effect_summaries_file = s;
    }
    jw.get("escape_summaries", "", s);
    if (s != "") {
      m_external_escape_summaries_file = s;
    }
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  boost::optional<std::string> m_external_side_effect_summaries_file;
  boost::optional<std::string> m_external_escape_summaries_file;
};
