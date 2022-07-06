/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnalysisUsage.h"

#include "Pass.h"

AnalysisID get_analysis_id_by_pass(const Pass* pass) {
  return typeid(*pass).name();
}

void AnalysisUsage::do_pass_invalidation(
    std::unordered_map<AnalysisID, Pass*>* preserved_analysis_passes) const {

  if (m_preserve_all) {
    return;
  }

  std::unordered_set<AnalysisID> erase_list;
  // Invalidate existing preserved analyses.
  for (const auto& entry : *preserved_analysis_passes) {
    AnalysisID id = entry.first;
    if (m_preserve_specific.count(id)) {
      continue;
    }

    Pass* pass = entry.second;
    // `pass` may be null in an invalidation dry run for assertion purposes.
    if (pass) {
      pass->destroy_analysis_result();
    }
    erase_list.emplace(id);
  }

  for (const auto& to_erase : erase_list) {
    preserved_analysis_passes->erase(to_erase);
  }
}

void AnalysisUsage::check_dependencies(const std::vector<Pass*>& passes) {
  std::unordered_map<AnalysisID, Pass*> preserved_passes;
  std::ostringstream error;
  bool has_error = false;
  for (const Pass* pass : passes) {
    if (pass->is_analysis_pass()) {
      preserved_passes.emplace(get_analysis_id_by_pass(pass), nullptr);
    }

    AnalysisUsage analysis_usage;
    pass->set_analysis_usage(analysis_usage);

    const auto& required_passes = analysis_usage.get_required_passes();
    for (const auto& required_pass : required_passes) {
      if (!preserved_passes.count(required_pass)) {
        if (!has_error) {
          has_error = true;
        } else {
          // make a line break between errors.
          error << std::endl;
        }
        error << required_pass << " is required by " << pass->name();
      }
    }

    analysis_usage.do_pass_invalidation(&preserved_passes);
  }

  always_assert_type_log(!has_error, RedexError::UNSATISFIED_ANALYSIS_PASS,
                         "Unsatisfied analysis pass dependencies:\n%s",
                         error.str().c_str());
}
