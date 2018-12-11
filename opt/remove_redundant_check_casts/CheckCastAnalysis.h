/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "MonotonicFixpointIterator.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

namespace check_casts {

namespace impl {

using register_t = uint32_t;

using Domain = sparta::ConstantAbstractDomain<const DexType*>;

using Environment =
    sparta::PatriciaTreeMapAbstractEnvironment<register_t, Domain>;

class CheckCastAnalysis final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               Environment> {
 public:
  explicit CheckCastAnalysis(cfg::ControlFlowGraph* cfg, DexMethod* method)
      : MonotonicFixpointIterator(*cfg, cfg->blocks().size()),
        m_method(method) {}

  void analyze_node(const NodeId& node, Environment* env) const override;

  Environment analyze_edge(
      cfg::Edge* const& edge,
      const Environment& exit_state_at_source) const override;

  std::unordered_map<IRInstruction*, boost::optional<IRInstruction*>>
  collect_redundant_checks_replacement();

 private:
  void analyze_instruction(IRInstruction* insn, Environment* env) const;
  const DexMethod* m_method;
};

} // namespace impl

} // namespace check_casts
