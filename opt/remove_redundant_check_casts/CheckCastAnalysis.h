/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "CheckCastConfig.h"
#include "ControlFlow.h"
#include "TypeInference.h"

namespace check_casts {

namespace impl {

struct CheckCastReplacementItem {
  cfg::Block* block;
  IRInstruction* insn;
  boost::optional<IRInstruction*> replacement_insn;
  boost::optional<DexType*> replacement_type;

  CheckCastReplacementItem(cfg::Block* block,
                           IRInstruction* insn,
                           boost::optional<IRInstruction*> replacement_insn,
                           boost::optional<DexType*> replacement_type)
      : block(block),
        insn(insn),
        replacement_insn(replacement_insn),
        replacement_type(replacement_type) {
    always_assert(!replacement_insn || !replacement_type);
  }
};

using CheckCastReplacements = std::vector<CheckCastReplacementItem>;

class CheckCastAnalysis {

 public:
  explicit CheckCastAnalysis(const CheckCastConfig& config, DexMethod* method);
  CheckCastReplacements collect_redundant_checks_replacement() const;

 private:
  DexType* get_type_demand(IRInstruction* insn, size_t src_index) const;
  DexType* weaken_to_demand(IRInstruction* insn,
                            DexType* type,
                            bool weaken_to_not_interfacy) const;
  bool is_check_cast_redundant(IRInstruction* insn, DexType* check_type) const;
  type_inference::TypeInference* get_type_inference() const;
  bool can_catch_class_cast_exception(cfg::Block* block) const;

  DexType* m_class_cast_exception_type;
  DexMethod* m_method;
  using InstructionTypeDemands =
      std::unordered_map<IRInstruction*, std::unordered_set<DexType*>>;
  std::unique_ptr<InstructionTypeDemands> m_insn_demands;
  std::vector<cfg::InstructionIterator> m_check_cast_its;
  mutable std::unique_ptr<type_inference::TypeInference> m_type_inference;
};

} // namespace impl

} // namespace check_casts
