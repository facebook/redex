/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "ControlFlow.h"

namespace check_casts {

namespace impl {

struct CheckCastReplacementItem {
  cfg::Block* block;
  IRInstruction* insn;
  boost::optional<IRInstruction*> replacement;

  CheckCastReplacementItem(cfg::Block* block,
                           IRInstruction* insn,
                           boost::optional<IRInstruction*> replacement)
      : block(block), insn(insn), replacement(replacement) {}
};

using CheckCastReplacements = std::vector<CheckCastReplacementItem>;

class CheckCastAnalysis {

 public:
  explicit CheckCastAnalysis(DexMethod* method) : m_method(method){};
  CheckCastReplacements collect_redundant_checks_replacement();

 private:
  DexMethod* m_method;
};

} // namespace impl

} // namespace check_casts
