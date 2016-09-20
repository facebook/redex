/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Peephole.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "DexClass.h"
#include "DexInstruction.h"
#include "PassManager.h"
#include "Transform.h"
#include "DexUtil.h"
#include "Walkers.h"

namespace {
const DexMethod* method_Class_getSimpleName() {

  static auto const ret = DexMethod::make_method("Ljava/lang/Class;",
                                                "getSimpleName",
                                                "Ljava/lang/String;",
                                                {}); // Return type and params
  return ret;
}

DexString* get_simple_name(const DexType* type) {
  std::string full(type->get_name()->c_str());
  auto lpos = full.rfind('/');
  auto simple = full.substr(lpos + 1, full.size() - lpos - 2);
  return DexString::make_string(simple.c_str(), (uint32_t) simple.size());
}

/**
 * Peephole optimizations find dataflow patterns within a single basic block
 * and replace them with faster patterns.  The general pattern is to replace
 * the final operation with a faster one, which may no longer depend on all the
 * intermediate instructions in the pattern.  The peepholer doesn't clean up
 * these intermediate instructions, under the assumption that dead code
 * elimination will do so later.
 */
class PeepholeOptimizer {
 private:
  using RegWriters = std::vector<ssize_t>;
  using DataflowSources =
      std::vector<std::pair<DexInstruction*, std::vector<ssize_t>>>;

  const ssize_t kInvalid = -1;

  const std::vector<DexClass*>& m_scope;
  std::unordered_map<DexInstruction*, DexInstruction*> m_replacements;
  ssize_t m_last_call;
  RegWriters m_last_writer;
  DataflowSources m_dataflow_sources;
  int m_stats_check_casts_removed;
  int m_stats_check_casts_super_removed;
  int m_stats_simple_name;

  /**
   * Explicitly clear the dataflow analysis structures to (a) avoid passing
   * them around all over the place, and (b) avoid re-allocating them for every
   * block.
   */
  void init_dataflow(int16_t regs) {
    m_last_call = kInvalid;
    m_last_writer.resize(regs);
    std::fill(m_last_writer.begin(), m_last_writer.end(), kInvalid);
    m_dataflow_sources.clear();
  }

  DexInstruction* peephole_patterns(DexInstruction* insn) {
    if (is_move_result(insn->opcode())) {
      /*
       * const-class vA, Lsome/Class;
       * invoke-virtual {vA} Ljava/lang/Class;.getSimpleName()
       * move-result vB
       */
      if (m_last_call < 0) {
        return insn;
      }
      auto invokep = m_dataflow_sources[m_last_call];
      auto invoke = static_cast<DexOpcodeMethod*>(invokep.first);
      auto const& invoke_srcs = invokep.second;
      if (invoke->get_method() != method_Class_getSimpleName()) {
        return insn;
      }
      if (invoke_srcs[0] < 0) {
        return insn;
      }
      auto const_class = m_dataflow_sources[invoke_srcs[0]].first;
      if (const_class->opcode() != OPCODE_CONST_CLASS) {
        return insn;
      }
      auto clstype = static_cast<DexOpcodeType*>(const_class)->get_type();
      m_stats_simple_name++;
      return (new DexOpcodeString(OPCODE_CONST_STRING,
                                  get_simple_name(clstype)))
          ->set_dest(insn->dest());
    }

    if (insn->opcode() == OPCODE_CHECK_CAST) {
      /*
       * invoke-virtual Lsome/Class;
       * move-result vA;
       * check-cast vA, Lsome/Class;
       *
       */
      auto move_result_idx = m_last_writer[insn->src(0)];
      if (move_result_idx == kInvalid) {
        return insn;
      }
      auto move_resultp = m_dataflow_sources[move_result_idx];
      auto move_result = move_resultp.first;
      if (!is_move_result(move_result->opcode())) {
        return insn;
      }
      auto const& move_result_srcs = move_resultp.second;
      if (move_result_srcs[0] == kInvalid) {
        return insn;
      }
      auto invokep = m_dataflow_sources[move_result_srcs[0]];
      auto invoke = static_cast<DexOpcodeMethod*>(invokep.first);
      auto invoke_return_type = invoke->get_method()->get_proto()->get_rtype();
      auto check_type = static_cast<DexOpcodeType*>(insn)->get_type();
      if (check_type != invoke_return_type) {
        if (!check_cast(invoke_return_type, check_type)) {
          return insn;
        }
        m_stats_check_casts_super_removed++;
      }
      m_stats_check_casts_removed++;
      return (new DexInstruction(OPCODE_NOP));
    }
    return insn;
  }

  /**
   * Build a dataflow graph for this block.  Keep track of the last writer of
   * each register and use that to compute the source instructions for each
   * subsequent instruction.  (NB: we could do away with storing the sources if
   * the patterns were only two instructions deep.)  For compactness we use
   * vectors indexed by opcode position rather than maps.
   */
  void peephole_block(Block* block, uint16_t regs) {
    init_dataflow(regs);
    size_t index = 0;
    for (auto& mei : *block) {
      if (mei.type == MFLOW_OPCODE) {
        auto newop = peephole_patterns(mei.insn);
        if (newop != mei.insn) {
          m_replacements.emplace(mei.insn, newop);
        }
        if (is_invoke(newop->opcode())) {
          m_last_call = index;
        }
        m_dataflow_sources.push_back(
            std::make_pair(newop, std::vector<ssize_t>()));
        auto& sources = m_dataflow_sources.back().second;
        if (is_move_result(newop->opcode())) {
          sources.push_back(m_last_call);
        }
        for (unsigned i = 0; i < newop->srcs_size(); ++i) {
          sources.push_back(m_last_writer[newop->src(i)]);
        }
        if (newop->dests_size()) {
          m_last_writer[newop->dest()] = index;
        }
        ++index;
      }
    }
  }

  void apply_peepholes(MethodTransform* transform) {
    for (auto rep : m_replacements) {
      transform->replace_opcode(rep.first, rep.second);
    }
  }

  void peephole(DexMethod* method) {
    auto transform =
        MethodTransform::get_method_transform(method, true /* want_cfg */);
    m_replacements.clear();
    auto const& blocks = transform->cfg();
    for (auto const& block : blocks) {
      peephole_block(block, method->get_code()->get_registers_size());
    }
    apply_peepholes(transform);
  }

  void print_stats() {
    TRACE(PEEPHOLE, 1,
            "%d SimpleClassName instances removed \n", m_stats_simple_name);
    TRACE(PEEPHOLE, 1,
            "%d redundant check-cast instances removed \n",
            m_stats_check_casts_removed);
    TRACE(PEEPHOLE, 1,
            "%d redundant check-cast instances from super removed \n",
            m_stats_check_casts_super_removed);
  }

 public:
  PeepholeOptimizer(const std::vector<DexClass*>& scope)
      : m_scope(scope),
        m_stats_check_casts_removed(0),
        m_stats_check_casts_super_removed(0),
        m_stats_simple_name(0) {}

  void run() {
    walk_methods(m_scope,
                 [&](DexMethod* m) {
                   if (m->get_code()) {
                     peephole(m);
                   }
                 });
    print_stats();
  }
};
}

////////////////////////////////////////////////////////////////////////////////

void PeepholePass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  PeepholeOptimizer(scope).run();
}

static PeepholePass s_pass;
