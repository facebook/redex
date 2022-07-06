/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Assessments collect non-functional quality metrics, e.g. regarding the
 * quality of debug positions.
 */

#pragma once

#include <cstdint>
#include <string>

#include "Debug.h"
#include "DexClass.h"

namespace assessments {

using DexAssessment = std::unordered_map<std::string, uint64_t>;
using DexAssessmentItem = std::pair<std::string, uint64_t>;

std::vector<DexAssessmentItem> order(const DexAssessment&);
std::string to_string(const DexAssessment&);

class DexScopeAssessor final {
 public:
  explicit DexScopeAssessor(const Scope& scope) : m_scope(scope) {}
  DexAssessment run();

 private:
  const Scope& m_scope;
};

} // namespace assessments
