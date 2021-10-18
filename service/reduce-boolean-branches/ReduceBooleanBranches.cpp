/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This optimization reduces the instructions needed to express certain boolean
 * operations. In particular, written as Java for compactness:
 *
 * // reduce_diamonds
 * b != false ? true : false   ==>  b
 * b != false ? false : true   ==>  !b
 * b == false ? true : false   ==>  !b
 * b == false ? false : true   ==>  b
 * o != null  ? true : false   ==>  o instanceof Object
 * o != null  ? false : true   ==>  !(o instanceof Object)
 *
 * // reduce_xors
 * !!b                         ==> b
 * (!b) != false               ==> b == false
 * (!b) == false               ==> b == true
 *
 * Where...
 * - "b" is a Boolean
 * - "o" is an Object
 * - "!b" is encoded as "b xor 1"
 * - ?: is encoded a branching diamond pattern
 */

#include "ReduceBooleanBranches.h"

#include "CFGMutation.h"
#include "ReachingDefinitions.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"

namespace {
enum class AnalysisResult {
  None = 0,
  Boolean = 1,
  Object = 2,
  Unknown = 3,
};
AnalysisResult operator|(AnalysisResult a, AnalysisResult b) {
  return static_cast<AnalysisResult>(static_cast<int>(a) | static_cast<int>(b));
}
class Analyzer {
 private:
  bool m_is_static;
  DexTypeList* m_args;
  cfg::ControlFlowGraph& m_cfg;
  std::unique_ptr<reaching_defs::MoveAwareFixpointIterator>
      m_reaching_defs_fp_iter;

 public:
  Analyzer(bool is_static, DexTypeList* args, cfg::ControlFlowGraph& cfg)
      : m_is_static(is_static), m_args(args), m_cfg(cfg) {}
  AnalysisResult analyze(DexType* type) {
    if (type::is_boolean(type)) {
      return AnalysisResult::Boolean;
    } else if (type::is_object(type)) {
      return AnalysisResult::Object;
    } else {
      return AnalysisResult::Unknown;
    }
  }

  AnalysisResult analyze(cfg::Block* block, IRInstruction* insn, reg_t src) {
    auto defs = get_defs(block, insn, src);
    if (defs.is_top() || defs.is_bottom()) {
      // Shouldn't happen, but we are not going to fight that here
      return AnalysisResult::Unknown;
    }
    AnalysisResult result{AnalysisResult::None};
    for (IRInstruction* def : defs.elements()) {
      auto def_opcode = def->opcode();
      switch (def_opcode) {
      case OPCODE_IGET_BOOLEAN:
      case OPCODE_AGET_BOOLEAN:
      case OPCODE_SGET_BOOLEAN:
      case OPCODE_INSTANCE_OF:
        result = result | AnalysisResult::Boolean;
        break;
      case OPCODE_CONST_STRING:
      case OPCODE_CONST_CLASS:
      case OPCODE_NEW_INSTANCE:
      case OPCODE_IGET_OBJECT:
      case OPCODE_AGET_OBJECT:
      case OPCODE_SGET_OBJECT:
      case OPCODE_CHECK_CAST:
      case IOPCODE_LOAD_PARAM_OBJECT:
        result = result | AnalysisResult::Object;
        break;
      case IOPCODE_LOAD_PARAM: {
        int param_index{0};
        for (const auto& mie :
             InstructionIterable(m_cfg.get_param_instructions())) {
          if (mie.insn == def) {
            break;
          }
          param_index++;
        }
        if (!m_is_static && param_index-- == 0) {
          result = result | AnalysisResult::Object;
        } else {
          result = result | analyze(m_args->at(param_index));
        }
        break;
      }
      case OPCODE_INVOKE_SUPER:
      case OPCODE_INVOKE_INTERFACE:
      case OPCODE_INVOKE_STATIC:
      case OPCODE_INVOKE_DIRECT:
      case OPCODE_INVOKE_VIRTUAL: {
        auto rtype = def->get_method()->get_proto()->get_rtype();
        result = result | analyze(rtype);
        break;
      }
      default:
        // TODO: Analyze other common opcodes, and try to support them.
        // In particular, Boolean or/and/xor
        TRACE(RBB, 2, "Don't know type: %s", SHOW(def->opcode()));
        return AnalysisResult::Unknown;
      }
    }
    always_assert(result != AnalysisResult::None);
    return result;
  }

  cfg::InstructionIterator get_boolean_root(cfg::Block* block,
                                            IRInstruction* insn,
                                            size_t* negations) {
    always_assert(
        insn->opcode() == OPCODE_IF_EQZ || insn->opcode() == OPCODE_IF_NEZ ||
        (insn->opcode() == OPCODE_XOR_INT_LIT8 && insn->get_literal() == 1));

    *negations = 0;
    auto it = m_cfg.find_insn(insn, block);
    while (true) {
      auto defs = get_defs(it.block(), it->insn, it->insn->src(0));
      if (defs.is_top() || defs.is_bottom()) {
        // Shouldn't happen, but we are not going to fight that here
        break;
      }
      if (defs.size() > 1) {
        break;
      }
      auto xor_1_insn = *defs.elements().begin();
      if (xor_1_insn->opcode() != OPCODE_XOR_INT_LIT8 ||
          xor_1_insn->get_literal() != 1) {
        break;
      }
      it = m_cfg.find_insn(xor_1_insn, it.block());
      (*negations)++;
    }
    if (analyze(it.block(), it->insn, it->insn->src(0)) !=
        AnalysisResult::Boolean) {
      return InstructionIterable(m_cfg).end();
    }
    return it;
  }

 private:
  sparta::PatriciaTreeSetAbstractDomain<IRInstruction*> get_defs(
      cfg::Block* block, IRInstruction* insn, reg_t src) {
    if (!m_reaching_defs_fp_iter) {
      m_reaching_defs_fp_iter.reset(
          new reaching_defs::MoveAwareFixpointIterator(m_cfg));
      m_reaching_defs_fp_iter->run({});
    }

    auto env = m_reaching_defs_fp_iter->get_entry_state_at(block);
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it->insn != insn;
         m_reaching_defs_fp_iter->analyze_instruction(it++->insn, &env)) {
    }
    return env.get(src);
  }
};
} // namespace

namespace reduce_boolean_branches_impl {

ReduceBooleanBranches::ReduceBooleanBranches(
    const Config& config,
    bool is_static,
    DexTypeList* args,
    IRCode* code,
    const std::function<void()>* on_change)
    : m_config(config),
      m_is_static(is_static),
      m_args(args),
      m_code(code),
      m_on_change(on_change) {}

bool ReduceBooleanBranches::run() {
  bool any_changes = false;
  if (reduce_diamonds()) {
    any_changes = true;
  }
  if (reduce_xors()) {
    any_changes = true;
  }
  return any_changes;
}

bool ReduceBooleanBranches::reduce_diamonds() {
  auto& cfg = m_code->cfg();
  Analyzer analyzer(m_is_static, m_args, cfg);
  struct Reduction {
    IRInstruction* last_insn;
    cfg::Block* block;
    std::vector<IRInstruction*> replacement_insns;
    cfg::Edge* goto_edge;
    cfg::Block* new_goto_target;
  };
  std::vector<Reduction> reductions;
  for (auto block : cfg.blocks()) {
    if (block->branchingness() != opcode::BRANCH_IF) {
      continue;
    }
    auto last_insn_it = block->get_last_insn();
    always_assert(last_insn_it != block->end());
    auto last_insn = last_insn_it->insn;
    auto last_insn_opcode = last_insn->opcode();
    always_assert(opcode::is_branch(last_insn_opcode));
    if (last_insn_opcode != OPCODE_IF_EQZ &&
        last_insn_opcode != OPCODE_IF_NEZ) {
      continue;
    }

    auto goto_edge = cfg.get_succ_edge_of_type(block, cfg::EDGE_GOTO);
    always_assert(goto_edge != nullptr);
    auto branch_edge = cfg.get_succ_edge_of_type(block, cfg::EDGE_BRANCH);
    always_assert(branch_edge != nullptr);

    auto goto_target = goto_edge->target();
    auto branch_target = branch_edge->target();
    auto goto_target_goto_edge =
        cfg.get_succ_edge_of_type(goto_target, cfg::EDGE_GOTO);
    auto branch_target_goto_edge =
        cfg.get_succ_edge_of_type(branch_target, cfg::EDGE_GOTO);
    if (goto_target_goto_edge == nullptr ||
        branch_target_goto_edge == nullptr) {
      continue;
    }

    auto goto_target_goto_edge_target = goto_target_goto_edge->target();
    auto branch_target_goto_edge_target = branch_target_goto_edge->target();
    if (goto_target_goto_edge_target != branch_target_goto_edge_target) {
      continue;
    }

    auto find_singleton_const_insn = [](cfg::Block* b) -> const IRInstruction* {
      const IRInstruction* insn = nullptr;
      for (const auto& mie : InstructionIterable(b)) {
        if (insn != nullptr) {
          return nullptr;
        }
        insn = mie.insn;
      }
      if (insn == nullptr || insn->opcode() != OPCODE_CONST) {
        return nullptr;
      }
      return insn;
    };
    auto goto_const_insn = find_singleton_const_insn(goto_target);
    auto branch_const_insn = find_singleton_const_insn(branch_target);
    if (goto_const_insn == nullptr || branch_const_insn == nullptr) {
      continue;
    }
    always_assert(goto_const_insn->opcode() == OPCODE_CONST);
    always_assert(branch_const_insn->opcode() == OPCODE_CONST);

    auto dest = goto_const_insn->dest();
    if (branch_const_insn->dest() != dest) {
      continue;
    }

    auto goto_literal = goto_const_insn->get_literal();
    auto branch_literal = branch_const_insn->get_literal();
    if ((goto_literal | branch_literal) != 1 ||
        (goto_literal & branch_literal) != 0) {
      continue;
    }

    auto src = last_insn->src(0);
    auto src_kind = analyzer.analyze(block, last_insn, src);
    auto full_removal =
        goto_target->preds().size() + branch_target->preds().size() == 2;
    std::vector<IRInstruction*> replacement_insns;
    if (src_kind == AnalysisResult::Boolean) {
      IRInstruction* replacement_insn;
      if ((last_insn_opcode == OPCODE_IF_EQZ) == (branch_literal == 0)) {
        replacement_insn = new IRInstruction(OPCODE_MOVE);
      } else {
        replacement_insn = new IRInstruction(OPCODE_XOR_INT_LIT8);
        replacement_insn->set_literal(1);
      }
      replacement_insn->set_dest(dest)->set_src(0, src);
      replacement_insns.push_back(replacement_insn);
      m_stats.boolean_branches_removed++;
    } else if (src_kind == AnalysisResult::Object && full_removal) {
      auto instance_of_insn = new IRInstruction(OPCODE_INSTANCE_OF);
      instance_of_insn->set_type(type::java_lang_Object());
      instance_of_insn->set_src(0, src);
      replacement_insns.push_back(instance_of_insn);
      auto move_result_pseudo_insn =
          new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO);
      move_result_pseudo_insn->set_dest(dest);
      replacement_insns.push_back(move_result_pseudo_insn);
      if ((last_insn_opcode == OPCODE_IF_EQZ) != (branch_literal == 0)) {
        auto xor_insn = new IRInstruction(OPCODE_XOR_INT_LIT8);
        xor_insn->set_literal(1)->set_dest(dest)->set_src(0, dest);
        replacement_insns.push_back(xor_insn);
      }
      m_stats.object_branches_removed++;
    } else {
      continue;
    }
    reductions.push_back({last_insn, block, replacement_insns, goto_edge,
                          goto_target_goto_edge_target});
  }
  if (!reductions.empty()) {
    if (m_on_change) {
      (*m_on_change)();
    }
    for (auto& r : reductions) {
      auto it = cfg.find_insn(r.last_insn, r.block);
      always_assert(!it.is_end());
      cfg.replace_insns(it, r.replacement_insns);
      cfg.set_edge_target(r.goto_edge, r.new_goto_target);
    }
    cfg.simplify();
    return true;
  }
  return false;
}

bool ReduceBooleanBranches::reduce_xors() {
  auto& cfg = m_code->cfg();
  struct Reduction {
    IRInstruction* insn;
    IROpcode new_op;
    reg_t new_src;
  };
  Analyzer analyzer(m_is_static, m_args, cfg);
  std::vector<Reduction> reductions;
  cfg::CFGMutation mutation(cfg);
  for (auto block : cfg.blocks()) {
    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (insn->opcode() != OPCODE_IF_EQZ && insn->opcode() != OPCODE_IF_NEZ &&
          (insn->opcode() != OPCODE_XOR_INT_LIT8 || insn->get_literal() != 1)) {
        // TOOD: Support more scenarios, e.g. reducing a double-xored value
        // flowing into a Boolean field store.
        continue;
      }

      size_t negations;
      auto root = analyzer.get_boolean_root(block, insn, &negations);
      if (root.is_end() || negations == 0) {
        continue;
      }

      auto temp_reg = cfg.allocate_temp();
      auto move_insn = new IRInstruction(OPCODE_MOVE);
      move_insn->set_dest(temp_reg)->set_src(0, root->insn->src(0));
      mutation.insert_before(root, {move_insn});
      IROpcode new_op = (negations & 1) == 0 ? insn->opcode()
                        : insn->opcode() == OPCODE_XOR_INT_LIT8
                            ? OPCODE_MOVE
                            : opcode::invert_conditional_branch(insn->opcode());
      reductions.push_back({insn, new_op, temp_reg});
      m_stats.xors_reduced++;
    }
  }
  if (!reductions.empty()) {
    if (m_on_change) {
      (*m_on_change)();
    }
    for (auto& r : reductions) {
      r.insn->set_opcode(r.new_op)->set_src(0, r.new_src);
    }
  }
  mutation.flush();
  return !reductions.empty();
}

Stats& Stats::operator+=(const Stats& that) {
  boolean_branches_removed += that.boolean_branches_removed;
  object_branches_removed += that.object_branches_removed;
  xors_reduced += that.xors_reduced;
  return *this;
}

} // namespace reduce_boolean_branches_impl
