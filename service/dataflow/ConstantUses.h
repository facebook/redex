/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ReachingDefinitions.h"
#include "TypeInference.h"
#include <functional>

namespace constant_uses {

enum TypeDemand : uint8_t {
  Error = 0x00,
  Int = 0x01,
  Float = 0x02,
  Long = 0x04,
  Double = 0x08,
  Object = 0x10,

  IntOrFloat = Int | Float,
  IntOrObject = Int | Object,
  LongOrDouble = Long | Double,
  None = (TypeDemand)(~0)
};

class ConstantUses {
 public:
  ConstantUses(const cfg::ControlFlowGraph& cfg, DexMethod* method);
  ConstantUses(const cfg::ControlFlowGraph& cfg,
               bool is_static,
               DexType* declaring_type,
               DexType* rtype,
               DexTypeList* args,
               const std::function<std::string()>& method_describer);

  // Given a const or const-wide instruction, retrieve all instructions that
  // use it.
  const std::vector<std::pair<IRInstruction*, size_t>>& get_constant_uses(
      IRInstruction*) const;

  // Given a const or const-wide instruction, compute the combination type
  // demand across all execution paths.
  TypeDemand get_constant_type_demand(IRInstruction*) const;

  // Whether type inference information was required to be computed.
  bool has_type_inference() const;

 private:
  static TypeDemand get_type_demand(DexType* type);
  TypeDemand get_type_demand(IRInstruction* insn, size_t src_index) const;

  mutable std::unique_ptr<type_inference::TypeInference> m_type_inference;
  reaching_defs::MoveAwareFixpointIterator m_reaching_definitions;
  std::unordered_map<IRInstruction*,
                     std::vector<std::pair<IRInstruction*, size_t>>>
      m_constant_uses;
  std::vector<std::pair<IRInstruction*, size_t>> m_no_uses;
  DexType* m_rtype;
};

} // namespace constant_uses
