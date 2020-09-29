/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"
#include "ScopedCFG.h"

namespace optimize_enums {

namespace impl {

// Forward declaration.
class FieldAnalyzer;
class ConstAnalyzer;

} // namespace impl

using GeneratedSwitchCases =
    std::unordered_map<DexField*, std::unordered_map<size_t, DexField*>>;

class OptimizeEnumsGeneratedAnalysis final {
 public:
  ~OptimizeEnumsGeneratedAnalysis();

  explicit OptimizeEnumsGeneratedAnalysis(const DexClass* generated_cls,
                                          const DexType* current_enum);

  void collect_generated_switch_cases(
      GeneratedSwitchCases& generated_switch_cases);

 private:
  const DexType* m_enum;
  const DexClass* m_generated_cls;
  cfg::ScopedCFG m_clinit_cfg;
  std::unique_ptr<impl::FieldAnalyzer> m_field_analyzer;
  std::unique_ptr<impl::ConstAnalyzer> m_const_analyzer;
};

} // namespace optimize_enums
