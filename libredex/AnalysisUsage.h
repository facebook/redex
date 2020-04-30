/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <typeinfo>
#include <unordered_set>

// An AnalysisID is a string that represent the type of the pass. It is derived
// from typeid(SomeAnalysisPass).name().
using AnalysisID = std::string;

/**
 * An object that is used to represent the analysis usage of a certain pass.
 * This information is provided by a pass to the Pass infrastructure through the
 * get_analysis_usage virtual function.
 *
 * Currently we support only preserving either all or none of the analyses. We
 * can implement more complicated rules in the future.
 */

class AnalysisUsage {
 public:
  void set_preserve_all() { m_preserve_all = true; }
  void set_preserve_none() { m_preserve_all = false; }

  bool get_preserve_status() const { return m_preserve_all; }

  // A required pass is used by (thus should precede) this current pass.
  template <typename AnalysisPassType>
  void add_required() {
    m_required_passes.emplace(typeid(AnalysisPassType).name());
  }

  // Returns a set of passes used by (thus should precede) this current pass.
  const std::unordered_set<AnalysisID>& get_required_passes() {
    return m_required_passes;
  }

 private:
  bool m_preserve_all = false;
  std::unordered_set<AnalysisID> m_required_passes;
};
