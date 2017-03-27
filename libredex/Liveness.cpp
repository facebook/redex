/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <deque>

#include "Dataflow.h"
#include "IRInstruction.h"
#include "Liveness.h"
#include "Transform.h"

//////////////////////////////////////////////////////////////////////////////

bool Liveness::operator==(const Liveness& that) const {
  return m_reg_set == that.m_reg_set;
}

void Liveness::enlarge(uint16_t ins_size, uint16_t newregs) {
  if (m_reg_set.size() < newregs) {
    auto oldregs = m_reg_set.size();
    m_reg_set.resize(newregs);
    for (uint16_t i = 0; i < ins_size; ++i) {
      m_reg_set[newregs - 1 - i] = m_reg_set[oldregs - 1 - i];
      m_reg_set[oldregs - 1 - i] = false;
    }
  }
}

void Liveness::trans(const IRInstruction* inst, Liveness* liveness) {
  if (inst->dests_size()) {
    liveness->m_reg_set.reset(inst->dest());
    if (inst->dest_is_wide()) {
      liveness->m_reg_set.reset(inst->dest() + 1);
    }
  }
  for (size_t i = 0; i < inst->srcs_size(); i++) {
    liveness->m_reg_set.set(inst->src((int)i));
    if (inst->src_is_wide((int)i)) {
      liveness->m_reg_set.set(inst->src((int)i) + 1);
    }
  }
  if (opcode::has_range(inst->opcode())) {
    for (size_t i = 0; i < inst->range_size(); i++) {
      liveness->m_reg_set.set(inst->range_base() + i);
    }
  }
}

void Liveness::meet(const Liveness& that) {
  m_reg_set |= that.m_reg_set;
}

std::unique_ptr<LivenessMap> Liveness::analyze(ControlFlowGraph& cfg,
                                               uint16_t nregs) {
  TRACE(REG, 5, "%s\n", SHOW(cfg));
  auto blocks = postorder_sort(cfg.blocks());
  auto liveness = backwards_dataflow<Liveness>(blocks, Liveness(nregs),
      Liveness::trans);

  auto DEBUG_ONLY dump_liveness = [&](const LivenessMap& amap) {
    for (auto& block : cfg.blocks()) {
      for (auto& mie : *block) {
        if (mie.type != MFLOW_OPCODE) {
          continue;
        }
        auto& analysis = amap.at(mie.insn);
        TRACE(REG, 5, "%04x: %s", mie.addr, SHOW(mie.insn));
        TRACE(REG, 5, " [Live registers:%s]\n", SHOW(analysis));
      }
    }
    return "";
  };
  TRACE(REG, 5, "%s", dump_liveness(*liveness));

  return liveness;
}
