/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MatchFlow.h"
#include "Show.h"
#include "Trace.h"

#include <ostream>
#include <unordered_set>

namespace mf {

result_t flow_t::find(cfg::ControlFlowGraph& cfg, location_t l) const {
  return find(cfg, {l});
}

result_t flow_t::find(cfg::ControlFlowGraph& cfg,
                      std::initializer_list<location_t> ls) const {
  std::unordered_set<detail::LocationIx> lixs(ls.size());
  for (auto l : ls) {
    always_assert(this == l.m_owner && "location_t from another flow_t");
    lixs.insert(l.m_ix);
  }

  TRACE(MFLOW, 6, "find: Building Instruction Graph");
  auto dfg = detail::instruction_graph(cfg, m_constraints, lixs);

  TRACE(MFLOW, 6, "find: Propagating Flow Constraints");
  dfg.propagate_flow_constraints(m_constraints);

  TRACE(MFLOW, 6, "find: Done.");
  return result_t{dfg.locations(lixs)};
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

std::ostream& operator<<(std::ostream& os, const result_t& res) {
  size_t iix = 0;
  std::unordered_map<const IRInstruction*, size_t> iids;
  const auto insn_id = [&iix, &iids](const IRInstruction* insn) {
    auto r = iids.emplace(insn, iix);
    if (r.second) {
      ++iix;
    }
    return r.first->second;
  };

  size_t lix = 0;
  for (const auto& insns : res.m_results) {
    os << "L" << lix++ << ":\n";
    if (!insns) {
      continue;
    }

    for (const auto& insn_to_srcs : *insns) {
      const auto* insn = insn_to_srcs.first;
      const auto& srcs = insn_to_srcs.second;

      os << "  I" << insn_id(insn) << ": " << show(insn) << "\n";

      for (size_t six = 0; six < srcs.size(); ++six) {
        auto& src = srcs[six];
        if (src.empty()) {
          continue;
        }

        os << "    S" << six << " <-";
        for (const auto* from : src) {
          os << " I" << insn_id(from);
        }
        os << "\n";
      }
    }
  }

  return os;
}

} // namespace mf
