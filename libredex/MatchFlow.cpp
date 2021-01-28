/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MatchFlow.h"

namespace mf {

result_t flow_t::find(cfg::ControlFlowGraph& cfg, location_t l) const {
  always_assert(this == l.m_owner && "location_t from another flow_t");

  auto dfg = detail::instruction_graph(cfg, m_constraints, l.m_ix);
  dfg.propagate_flow_constraints(m_constraints);

  return result_t{dfg.locations(l.m_ix)};
}

result_t::insn_range result_t::matching(location_t l) const {
  if (l.m_ix >= m_results.size()) {
    return insn_range::empty();
  }

  auto& insns = m_results[l.m_ix];
  if (!insns) {
    return insn_range::empty();
  }

  return insn_range{insn_iterator{insns->cbegin()},
                    insn_iterator{insns->cend()}};
}

result_t::src_range result_t::matching(location_t l,
                                       const IRInstruction* insn,
                                       src_index_t ix) const {
  if (l.m_ix >= m_results.size()) {
    return src_range::empty();
  }

  auto& insns = m_results[l.m_ix];
  if (!insns) {
    return src_range::empty();
  }

  // const_cast is required because `insns` holds `IRInstruction*` keys, but
  // `find` will not mutate the `insn` passed in, despite the cast.
  auto srcs = insns->find(const_cast<IRInstruction*>(insn));
  if (srcs == insns->end() || ix >= srcs->second.size()) {
    return src_range::empty();
  }

  auto& src = srcs->second[ix];
  return src_range{src.cbegin(), src.cend()};
}

} // namespace mf
