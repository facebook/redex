/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagationV3.h"

#include "DexUtil.h"
#include "GlobalConstProp.h"
#include "LocalConstProp.h"
#include "Walkers.h"

using std::placeholders::_1;
using std::string;
using std::vector;

namespace {

/** Intraprocedural Constant propagation
 * This code leverages the analysis built by LocalConstantPropagation
 * with works at the basic block level and extends its capabilities by
 * leveraging the Abstract Interpretation Framework's FixPointIterator
 * and HashedAbstractEnvironment facilities.
 *
 * By running the fix point iterator, instead of having no knowledge at
 * the start of a basic block, we can now run the analsys with constants
 * that have been propagated beyond the basic block boundary making this
 * more powerful than its predecessor pass.
 */
class IntraProcConstantPropagation final
    : public ConstantPropFixpointAnalysis<Block*,
                                          MethodItemEntry,
                                          std::vector<Block*>,
                                          InstructionIterable> {
 public:
  explicit IntraProcConstantPropagation(ControlFlowGraph& cfg)
      : ConstantPropFixpointAnalysis<Block*,
                                     MethodItemEntry,
                                     vector<Block*>,
                                     InstructionIterable>(
            cfg.entry_block(),
            cfg.blocks(),
            std::bind(&Block::succs, _1),
            std::bind(&Block::preds, _1)) {}

  void simplify_instruction(
      Block* const& block,
      MethodItemEntry& mie,
      const ConstPropEnvironment& current_state) const override {
    auto insn = mie.insn;
    m_lcp.simplify_instruction(insn, current_state);
  }

  void analyze_instruction(const MethodItemEntry& mie,
                           ConstPropEnvironment* current_state) const override {
    auto insn = mie.insn;
    m_lcp.analyze_instruction(insn, current_state);
  }

  void apply_changes(DexMethod* method) const {
    auto code = method->get_code();
    for (auto const& p : m_lcp.branch_replacements()) {
      IRInstruction* const& old_op = p.first;
      IRInstruction* const& new_op = p.second;
      if (new_op->opcode() == OPCODE_NOP) {
        TRACE(CONSTP, 4, "Removing instruction %s\n", SHOW(old_op));
        code->remove_opcode(old_op);
        delete new_op;
      } else {
        TRACE(CONSTP,
              4,
              "Replacing instruction %s -> %s\n",
              SHOW(old_op),
              SHOW(new_op));
        code->replace_branch(old_op, new_op);
      }
    }
  }

  size_t branches_removed() const { return m_lcp.num_branch_propagated(); }

 private:
  mutable LocalConstantPropagation m_lcp;
};

} // namespace

void ConstantPropagationPassV3::configure_pass(const PassConfig& pc) {
  vector<string> blacklist_names;
  pc.get("blacklist", {}, blacklist_names);

  for (auto const& name : blacklist_names) {
    DexType* entry = DexType::get_type(name.c_str());
    if (entry) {
      TRACE(CONSTP, 2, "Blacklisted class: %s\n", SHOW(entry));
      m_blacklist.insert(entry);
    }
  }
}

void ConstantPropagationPassV3::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  auto scope = build_class_scope(stores);

  walk_methods(scope, [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }
    // Skipping blacklisted classes
    if (m_blacklist.count(method->get_class()) > 0) {
      TRACE(CONSTP, 2, "Skipping %s\n", SHOW(method));
      return;
    }

    TRACE(CONSTP, 5, "Class: %s\n", SHOW(method->get_class()));
    TRACE(CONSTP, 5, "Method: %s\n", SHOW(method->get_name()));

    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();

    TRACE(CONSTP, 5, "CFG: %s\n", SHOW(cfg));
    IntraProcConstantPropagation rcp(cfg);
    rcp.run(ConstPropEnvironment());
    rcp.simplify();
    rcp.apply_changes(method);

    m_branches_removed += rcp.branches_removed();
  });

  mgr.incr_metric("num_branch_propagated", m_branches_removed);
}

static ConstantPropagationPassV3 s_pass;
