/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Pass;

// An AnalysisID is a string that represent the type of the pass.
using AnalysisID = std::string;

template <typename AnalysisPass>
inline AnalysisID get_analysis_id_by_pass() {
  return typeid(AnalysisPass).name();
}

AnalysisID get_analysis_id_by_pass(const Pass* pass);

/**
 * An object that is used to represent the analysis usage of a certain pass.
 * This information is provided by a pass to the Pass infrastructure through the
 * get_analysis_usage virtual function.
 *
 * Currently we support only preserving either all, none, or specific analysis
 * passes.
 */

class AnalysisUsage {
 public:
  void set_preserve_all(bool preserve_all = true) {
    m_preserve_all = preserve_all;
  }

  // A required pass is used by (thus should precede) this current pass.
  template <typename AnalysisPassType>
  void add_required() {
    m_required_passes.emplace(get_analysis_id_by_pass<AnalysisPassType>());
  }

  // Declares that this current pass preserves a specific analysis pass.
  template <typename AnalysisPassType>
  void add_preserve_specific() {
    m_preserve_specific.emplace(get_analysis_id_by_pass<AnalysisPassType>());
  }

  // Returns a set of passes used by (thus should precede) this current pass.
  const std::unordered_set<AnalysisID>& get_required_passes() {
    return m_required_passes;
  }

  // Called from PassManager. Invalidates preserved pass according to the pass
  // invalidation policy set up by the pass in which the AnalysisUsage is
  // defined.
  void do_pass_invalidation(
      std::unordered_map<AnalysisID, Pass*>* preserved_analysis_passes) const;

  // Called from PassManager. Performs checks on analysis pass dependencies
  // without running any pass.
  static void check_dependencies(const std::vector<Pass*>& passes);

 private:
  bool m_preserve_all = false;
  std::unordered_set<AnalysisID> m_required_passes;
  std::unordered_set<AnalysisID> m_preserve_specific;
};
