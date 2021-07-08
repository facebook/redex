/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAssessments.h"

#include <ostream>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

// We define particular assessment implementations in their own namespaces.

namespace {

namespace dex_position {

struct Assessment {
  // Positions
  uint64_t methods_without_positions{0};
  uint64_t methods_with_unknown_source_positions{0};
  uint64_t blocks_outside_try_without_positions{0};
  uint64_t blocks_inside_try_without_positions{0};
  uint64_t dangling_parent_positions{0};
  uint64_t parent_position_cycles{0};
  uint64_t outlined_method_invocation_without_pattern_position{0};
  uint64_t pattern_position_without_outlined_method_invocation{0};
  uint64_t switch_positions_outside_outlined_methods{0};
  uint64_t pattern_positions_inside_outlined_methods{0};
  uint64_t positions{0};
  uint64_t switch_positions{0};
  uint64_t pattern_positions{0};
  uint32_t max_parent_depth{0};
  bool has_problems() {
    return blocks_outside_try_without_positions ||
           blocks_inside_try_without_positions || dangling_parent_positions ||
           outlined_method_invocation_without_pattern_position ||
           pattern_position_without_outlined_method_invocation ||
           switch_positions_outside_outlined_methods ||
           (!PositionPatternSwitchManager::
                CAN_OUTLINED_METHOD_INVOKE_OUTLINED_METHOD &&
            pattern_positions_inside_outlined_methods);
  }
  Assessment& operator+=(const Assessment& other) {
    methods_without_positions += other.methods_without_positions;
    methods_with_unknown_source_positions +=
        other.methods_with_unknown_source_positions;
    blocks_outside_try_without_positions +=
        other.blocks_outside_try_without_positions;
    blocks_inside_try_without_positions +=
        other.blocks_inside_try_without_positions;
    dangling_parent_positions += other.dangling_parent_positions;
    parent_position_cycles += other.parent_position_cycles;
    outlined_method_invocation_without_pattern_position +=
        other.outlined_method_invocation_without_pattern_position;
    pattern_position_without_outlined_method_invocation +=
        other.pattern_position_without_outlined_method_invocation;
    switch_positions_outside_outlined_methods +=
        other.switch_positions_outside_outlined_methods;
    pattern_positions_inside_outlined_methods +=
        other.pattern_positions_inside_outlined_methods;
    positions += other.positions;
    switch_positions += other.switch_positions;
    pattern_positions += other.pattern_positions;
    max_parent_depth = std::max(max_parent_depth, other.max_parent_depth);
    return *this;
  }

  assessments::DexAssessment to_dex_assessment() {
    assessments::DexAssessment res;
    // Positions
    res["methods_without_positions"] = methods_without_positions;
    res["methods_with_unknown_source_positions"] =
        methods_with_unknown_source_positions;
    res["blocks_outside_try_without_positions"] =
        blocks_outside_try_without_positions;
    res["blocks_inside_try_without_positions"] =
        blocks_inside_try_without_positions;
    res["dangling_parent_positions"] = dangling_parent_positions;
    res["parent_position_cycles"] = parent_position_cycles;
    res["outlined_method_invocation_without_pattern_position"] =
        outlined_method_invocation_without_pattern_position;
    res["pattern_position_without_outlined_method_invocation"] =
        pattern_position_without_outlined_method_invocation;
    res["switch_positions_outside_outlined_methods"] =
        switch_positions_outside_outlined_methods;
    res["pattern_positions_inside_outlined_methods"] =
        pattern_positions_inside_outlined_methods;
    res["positions"] = positions;
    res["switch_positions"] = switch_positions;
    res["pattern_positions"] = pattern_positions;
    res["max_parent_depth"] = max_parent_depth;
    return res;
  }
};

bool needs_position(IROpcode opcode) {
  if (!opcode::can_throw(opcode)) {
    return false;
  }
  if (opcode == OPCODE_CONST_STRING) {
    // javac and/or the dexer seem to systematically ignore const-string.
    return false;
  }
  if (opcode == OPCODE_NEW_ARRAY || opcode::is_an_aput(opcode)) {
    // javac and/or the dexer seem to systematically ignore certain
    // array-related instructions.
    return false;
  }
  if (opcode == OPCODE_MONITOR_ENTER || opcode == OPCODE_MONITOR_EXIT ||
      opcode == OPCODE_CONST_CLASS) {
    // javac and/or the dexer seem not provide positions for implicit
    // synchronization code of synchronized methods.
    return false;
  }
  if (opcode == OPCODE_INSTANCE_OF) {
    // inserted by VirtualMerging, and cannot actually throw
    return false;
  }
  return true;
}

class Assessor {
 private:
  PositionPatternSwitchManager* m_manager;
  DexString* m_unknown_source;

 public:
  Assessor()
      : m_manager(g_redex->get_position_pattern_switch_manager()),
        m_unknown_source(DexString::get_string("UnknownSource")) {}

  Assessment analyze_method(DexMethod* method, cfg::ControlFlowGraph& cfg) {
    Assessment assessment;
    auto is_outlined_method = method->rstate.outlined();
    std::unordered_set<DexPosition*> positions;
    std::unordered_set<DexPosition*> parents;
    bool any_unknown_source_position = false;
    // We are working with a *non-editable* cfg here. A key difference between
    // an editable and an uneditable cfg is that the latter has not been
    // enriched with trailing positions in all blocks
    // (ControlFlowGraph::find_block_boundaries), while linearlization (via
    // remove_duplicate_positions) removes redundant positions across block
    // boundaries. This, we keep track of last positions across blocks, just as
    // the cfg would when building an editable cfg (and just as symbolication
    // would when going backwards to find the position relevant to an
    // instruction offset).
    DexPosition* last_position = nullptr;
    for (auto block : cfg.blocks()) {
      bool block_without_position_reported = false;
      for (auto it = block->begin(); it != block->end(); it++) {
        if (it->type == MFLOW_POSITION) {
          positions.insert(it->pos.get());
          last_position = it->pos.get();
          if (last_position->line == 0 &&
              last_position->file == m_unknown_source) {
            any_unknown_source_position = true;
          }
        } else if (it->type == MFLOW_OPCODE) {
          auto insn = it->insn;
          if (!last_position && !block_without_position_reported &&
              needs_position(insn->opcode())) {
            if (cfg.get_succ_edge_of_type(block, cfg::EdgeType::EDGE_THROW)) {
              assessment.blocks_inside_try_without_positions++;
            } else {
              assessment.blocks_outside_try_without_positions++;
            }
            block_without_position_reported = true;
          }
          if (opcode::is_invoke_static(insn->opcode()) &&
              insn->get_method()->is_def() &&
              insn->get_method()->as_def()->rstate.outlined()) {
            if (!last_position ||
                !m_manager->is_pattern_position(last_position)) {
              assessment.outlined_method_invocation_without_pattern_position++;
            }
          } else if (last_position &&
                     m_manager->is_pattern_position(last_position) &&
                     opcode::may_throw(insn->opcode())) {
            assessment.pattern_position_without_outlined_method_invocation++;
          }
        }
      }
    }
    std::unordered_map<DexPosition*, uint32_t> parent_depths;
    std::function<uint32_t(DexPosition*)> get_parent_depth;
    get_parent_depth = [&](DexPosition* pos) -> uint32_t {
      if (pos == nullptr) {
        return 0;
      }
      auto it = parent_depths.find(pos);
      if (it != parent_depths.end()) {
        if (it->second < 0) {
          assessment.parent_position_cycles++;
          return 0;
        }
        return it->second;
      }
      if (!positions.count(pos)) {
        assessment.dangling_parent_positions++;
        return 0;
      }
      parent_depths.emplace(pos, -1);
      auto depth = get_parent_depth(pos->parent) + 1;
      parent_depths[pos] = depth;
      assessment.max_parent_depth =
          std::max(assessment.max_parent_depth, depth);
      return depth;
    };
    for (auto pos : positions) {
      get_parent_depth(pos->parent);
      if (m_manager->is_pattern_position(pos)) {
        assessment.pattern_positions++;
        if (is_outlined_method) {
          assessment.pattern_positions_inside_outlined_methods++;
        }
      } else if (m_manager->is_switch_position(pos)) {
        assessment.switch_positions++;
        if (!is_outlined_method) {
          assessment.switch_positions_outside_outlined_methods++;
        }
      }
    }
    if (positions.empty()) {
      assessment.methods_without_positions++;
      // we forgive the missing block positions
      assessment.blocks_inside_try_without_positions = 0;
      assessment.blocks_outside_try_without_positions = 0;
    } else if (any_unknown_source_position) {
      assessment.methods_with_unknown_source_positions++;
      // we forgive the missing block positions
      assessment.blocks_inside_try_without_positions = 0;
      assessment.blocks_outside_try_without_positions = 0;
    }
    assessment.positions += positions.size();
    return assessment;
  }
};

} // namespace dex_position

} // namespace

namespace assessments {

std::vector<DexAssessmentItem> order(const DexAssessment& assessment) {
  std::vector<DexAssessmentItem> res(assessment.begin(), assessment.end());
  std::sort(res.begin(),
            res.end(),
            [](const DexAssessmentItem& a, const DexAssessmentItem& b) {
              return a.first < b.first;
            });
  return res;
}

std::string to_string(const DexAssessment& assessment) {
  std::ostringstream oss;
  bool first = true;
  for (auto& p : order(assessment)) {
    if (p.second) {
      if (first) {
        first = false;
      } else {
        oss << ", ";
      }
      oss << p.first << ": " << p.second;
    }
  }
  return oss.str();
}

DexAssessment DexScopeAssessor::run() {
  // This struct combines all individual assessment implementations.
  struct Assessment {
    dex_position::Assessment dex_position_assessment;
    Assessment& operator+=(const Assessment& other) {
      dex_position_assessment += other.dex_position_assessment;
      return *this;
    }
    bool has_problems() { return dex_position_assessment.has_problems(); }
    DexAssessment to_dex_assessment() {
      return dex_position_assessment.to_dex_assessment();
    }
  };

  dex_position::Assessor dex_position_assessor;
  auto combined_assessment = walk::parallel::methods<Assessment>(
      m_scope, [&dex_position_assessor](DexMethod* method) {
        Assessment assessment;
        auto code = method->get_code();
        if (!code) {
          return assessment;
        }

        always_assert(!code->editable_cfg_built());
        if (!code->cfg_built()) {
          code->build_cfg(/*editable*/ false);
        }

        assessment.dex_position_assessment =
            dex_position_assessor.analyze_method(method, code->cfg());

        if (traceEnabled(ASSESSOR, 2) && assessment.has_problems()) {
          if (traceEnabled(ASSESSOR, 3)) {
            TRACE(ASSESSOR,
                  3,
                  "[scope assessor] %s: %s\n%s",
                  SHOW(method),
                  to_string(assessment.to_dex_assessment()).c_str(),
                  SHOW(code->cfg()));
          } else {
            TRACE(ASSESSOR,
                  2,
                  "[scope assessor] %s: %s",
                  SHOW(method),
                  to_string(assessment.to_dex_assessment()).c_str());
          }
        }

        return assessment;
      });

  auto res = combined_assessment.to_dex_assessment();
  if (combined_assessment.has_problems()) {
    TRACE(ASSESSOR, 1, "[scope assessor] %s", to_string(res).c_str());
  }
  return res;
}

} // namespace assessments
