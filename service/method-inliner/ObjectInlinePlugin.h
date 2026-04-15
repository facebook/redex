/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CFGInliner.h"
#include "ControlFlow.h"

#include <optional>

using namespace cfg;
namespace object_inliner_plugin {

using FieldSet = UnorderedMap<reg_t, UnorderedSet<IRInstruction*>>;
using FieldSetMap = UnorderedMap<DexFieldRef*, FieldSet>;

class ObjectInlinePlugin : public CFGInlinerPlugin {

 public:
  ObjectInlinePlugin(
      FieldSetMap field_sets,
      const UnorderedMap<DexFieldRef*, DexFieldRef*>& field_swaps,
      std::vector<reg_t> srcs,
      std::optional<reg_t> value_register,
      std::optional<reg_t> caller_this,
      DexType* callee_type);

  /*
   * Copy the callee blocks into caller, fixing up self references, field
   * references (both set and get) based on FieldSetMap, parameter references,
   * which likely do not come from the insertion site, and indicate the
   * insertion site and a home for the final returned value of callee.
   */

  bool update_before_reg_remap(ControlFlowGraph* caller,
                               ControlFlowGraph* callee) override;
  bool update_after_reg_remap(ControlFlowGraph* caller,
                              ControlFlowGraph* callee) override;
  std::optional<std::reference_wrapper<const std::vector<reg_t>>> inline_srcs()
      override;
  std::optional<reg_t> reg_for_return() override;
  bool inline_after() override;
  bool remove_inline_site() override;

  void set_src_regs(std::vector<reg_t> srcs) { m_srcs = std::move(srcs); }

 private:
  // Fields set by the builder. This is provided as an argument.
  FieldSetMap m_initial_field_sets;
  // Used to record the register <-> field map. This is created as part of the
  // field removal in object inlining. All field reads and write will be
  // re-writen to use this.
  FieldSetMap m_set_field_sets;
  const UnorderedMap<DexFieldRef*, DexFieldRef*> m_field_swaps;
  std::vector<reg_t> m_srcs;
  std::optional<reg_t> m_value_reg;
  std::optional<reg_t> m_caller_this_reg;
};
} // namespace object_inliner_plugin
