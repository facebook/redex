/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CFGInliner.h"
#include "ClassInitCounter.h"
#include "ControlFlow.h"

#include <boost/optional.hpp>

using namespace cic;
using namespace cfg;

class ObjectInlinePlugin : public CFGInlinerPlugin {

 public:
  ObjectInlinePlugin(
      const FieldSetMap& field_sets,
      const std::map<DexFieldRef*, DexFieldRef*, dexfields_comparator>&
          field_swaps,
      const std::vector<reg_t>& srcs,
      reg_t value_register,
      boost::optional<reg_t> caller_this,
      reg_t callee_this,
      DexType* callee_type);

  /*
   * Copy the callee blocks into caller, fixing up self references, field
   * references (both set and get) based on FieldSetMap, parameter references,
   * which likely do not come from the insertion site, and indicate the
   * insertion site and a home for the final returned value of callee.
   */

  void update_before_reg_remap(ControlFlowGraph* caller,
                               ControlFlowGraph* callee) override;
  bool update_after_reg_remap(ControlFlowGraph* caller,
                              ControlFlowGraph* callee) override;
  const boost::optional<std::reference_wrapper<std::vector<reg_t>>>
  inline_srcs() override;
  boost::optional<reg_t> reg_for_return() override;
  bool inline_after() override;
  bool remove_inline_site() override;

  const FieldSetMap& get_unaccessed_sets() { return m_unaccessed_field_sets; }

 private:
  FieldSetMap m_initial_field_sets;
  FieldSetMap m_set_field_sets;
  FieldSetMap m_unaccessed_field_sets;
  const std::map<DexFieldRef*, DexFieldRef*, dexfields_comparator>
      m_field_swaps;
  std::vector<reg_t> m_srcs;
  reg_t m_value_reg;
  boost::optional<reg_t> m_caller_this_reg;
  reg_t m_callee_this_reg;
  DexType* m_callee_class;
};
