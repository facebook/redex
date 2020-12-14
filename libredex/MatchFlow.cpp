/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MatchFlow.h"

namespace mf {

result_t flow_t::find(const cfg::ControlFlowGraph&, location_t) const {
  detail::Locations locations{m_constraints.size()};
  return result_t{std::move(locations)};
}

result_t::insn_range result_t::matching(location_t l) const {
  auto& insns = m_results.at(l.m_ix);
  if (!insns) {
    return insn_range::empty();
  }

  return insn_range{insn_iterator{insns->cbegin()},
                    insn_iterator{insns->cend()}};
}

result_t::src_range result_t::matching(location_t l,
                                       const IRInstruction* insn,
                                       src_index_t ix) const {
  auto& insns = m_results.at(l.m_ix);
  if (!insns) {
    return src_range::empty();
  }

  // const_cast is required because `insns` holds `IRInstruction*` keys, but
  // `find` will not mutate the `insn` passed in, despite the cast.
  auto srcs = insns->find(const_cast<IRInstruction*>(insn));
  if (srcs == insns->end()) {
    return src_range::empty();
  }

  auto& src = srcs->second.at(ix);
  return src_range{src.cbegin(), src.cend()};
}

} // namespace mf
