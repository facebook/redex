/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE file in the root directory of this source tree. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include "ConstantPropagationV2.h"

#include <boost/optional.hpp>
#include <cmath>
#include <stack>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "Transform.h"
#include "Walkers.h"

/*
 * Propagate known values from `const` loads, through `move`s and `cmp`s,
 * to `if`s where the branch could be removed or replaced with a `goto`
 *
 * This pass operates on a method local level. However, it approaches basic
 * block boundaries in a very simple way. If there is more than one predecessor,
 * all constant information is dropped.
 */
namespace {

  // RegisterValues holds our compile-time knowledge of the register states.
  // Each register could be used solo (narrow) for 32 bit values.
  // Or, two registers can be used together as a wide (64 bit) value.
  //
  // This class makes sure you don't accidentally read half of a wide value,
  // and think it's a narrow value of its own.
  class RegisterValues {

   public:
    enum KnownState {
      UNKNOWN,
      KNOWN_NARROW,
      KNOWN_WIDE,
    };

    struct AbstractRegister {
      KnownState state;
      int32_t val;
    };

    // mark all regs to unknown
    void remove_constants() {
      for (auto &r: m_regs) {
        r.state = UNKNOWN;
      }
    }

    void reset(size_t size) {
      m_regs.clear();
      m_regs.reserve(size);
      for (size_t i = 0; i < size; i++) {
        m_regs.push_back({.state = UNKNOWN, .val = 0});
      }
    }

    boost::optional<int32_t> get(uint16_t r) {
      const AbstractRegister& reg = m_regs.at(r);
      if (reg.state == KNOWN_NARROW) {
        return reg.val;
      }
      return boost::none;
    }

    void put(int32_t value, uint16_t index) {
      AbstractRegister& reg = m_regs.at(index);
      reg.state = KNOWN_NARROW;
      reg.val = value;
    }

    boost::optional<int64_t> get_wide(uint16_t index) {
      const AbstractRegister& reg1 = m_regs.at(index);
      const AbstractRegister& reg2 = m_regs.at(index + 1);
      if (reg1.state == KNOWN_WIDE && reg2.state == KNOWN_WIDE) {
        int64_t upper = static_cast<int64_t>(reg1.val) << 32;
        int64_t lower = static_cast<int64_t>(reg2.val) & 0x00000000ffffffff;
        return upper | lower;
      }
      return boost::none;
    }

    void put_wide(int64_t value, uint16_t index) {
      AbstractRegister& reg1 = m_regs.at(index);
      AbstractRegister& reg2 = m_regs.at(index + 1);
      reg1.state = KNOWN_WIDE;
      reg2.state = KNOWN_WIDE;
      reg1.val = static_cast<int32_t>(value >> 32);
      reg2.val = static_cast<int32_t>(value);
    }

    void mark_unknown(uint16_t index) {
      m_regs.at(index).state = UNKNOWN;
    }

    void mark_unknown_wide(uint16_t index) {
      m_regs.at(index).state = UNKNOWN;
      m_regs.at(index + 1).state = UNKNOWN;
    }

    void move(uint16_t source, uint16_t dest) {
      auto src = m_regs.at(source);
      always_assert_log(src.state != KNOWN_WIDE, "move narrow on wide");
      m_regs.at(dest) = src;
    }

    void move_wide(uint16_t source, uint16_t dest) {
      auto first = m_regs.at(source);
      auto second = m_regs.at(source + 1);
      always_assert_log(first.state != KNOWN_NARROW &&
                            second.state != KNOWN_NARROW,
                        "move wide on narrow");
      m_regs.at(dest) = first;
      m_regs.at(dest + 1) = second;
    }

   private:
    std::vector<AbstractRegister> m_regs;
  };

  template <typename Size>
  boost::optional<Size> get_register(RegisterValues& reg_values, uint16_t register_index);

  template <>
  boost::optional<int64_t> get_register<int64_t>(
      RegisterValues& reg_values, uint16_t register_index) {
    return reg_values.get_wide(register_index);
  }

  template <>
  boost::optional<int32_t> get_register<int32_t>(
      RegisterValues& reg_values, uint16_t register_index) {
    return reg_values.get(register_index);
  }

  template <typename Size>
  void mark_unknown(RegisterValues& reg_values, uint16_t register_index);

  template <>
  void mark_unknown<int64_t>(
      RegisterValues& reg_values, uint16_t register_index) {
    reg_values.mark_unknown_wide(register_index);
  }

  template <>
  void mark_unknown<int32_t>(
      RegisterValues& reg_values, uint16_t register_index) {
    reg_values.mark_unknown(register_index);
  }

  constexpr const char* METRIC_BRANCH_PROPAGATED =
    "num_branch_propagated";

  class ConstantPropagation {
  private:
    const Scope& m_scope;
    const ConstantPropagationPassV2::Config& m_config;
    RegisterValues reg_values;
    std::vector<std::pair<IRInstruction*, IRInstruction*>> branch_replacements;
    size_t m_branch_propagated{0};

    void propagate(DexMethod* method) {
      reg_values.reset(method->get_code()->get_registers_size());

      bool changed = true;
      TRACE(CONSTP, 5, "Class: %s\n", SHOW(method->get_class()));
      TRACE(CONSTP, 5, "Method: %s\n", SHOW(method->get_name()));
      // The loop traverses all blocks in the method until no more branch is converted
      while (changed) {
        auto code = method->get_code();
        code->build_cfg();
        auto& cfg = code->cfg();
        TRACE(CONSTP, 5, "CFG: %s\n", SHOW(cfg));
        auto first_block = cfg.blocks()[0];
        // block_preds saves the re-calculated number of predecessors of each block
        // This number may change as new unreachable blocks are added in each loop
        std::unordered_map<Block*, int> block_to_predecessors_count;
        find_reachable_predecessors(cfg.blocks(), block_to_predecessors_count);
        changed = propagate_constant_in_method(
            method, first_block, block_to_predecessors_count);
        apply_changes(code);
      }
    }

    bool propagate_constant_in_method(
        DexMethod* method,
        Block* first_block,
        std::unordered_map<Block*, int>& block_preds) {
      bool changed = false;
      std::stack<Block*> dfs_front;
      dfs_front.push(first_block);
      std::unordered_map<Block*, bool> visited;
      visited[first_block] = true;
      // This loop traverses each block by depth-first
      while (!dfs_front.empty() && !changed) {
        auto current_block = dfs_front.top();
        dfs_front.pop();
        TRACE(CONSTP, 5, "Processing block %d\n", current_block->id());
        if (block_preds[current_block] != 1) {
          TRACE(CONSTP, 5, "More than one pred, removing constants\n");
          reg_values.remove_constants();
        }
        for (auto it = current_block->begin();
             it != current_block->end() && !changed;
             ++it) {
          if (it->type != MFLOW_OPCODE) {
            continue;
          }
          auto inst = it->insn;
          TRACE(CONSTP, 5, "instruction: %s\n", SHOW(inst));
          if (is_const(inst->opcode())) {
            propagate_constant(inst);
          } else {
            changed = propagate_insn(inst);
          }
        }
        // Propagate successive blocks
        int succ_num = 0;
        for (auto successor_block : current_block->succs()) {
          if (!visited[successor_block]) {
            dfs_front.push(successor_block);
            visited[successor_block] = true;
            succ_num++;
          }
        }
        if (succ_num != 1) {
          TRACE(CONSTP, 5, "More than one successor, removing constants\n");
          reg_values.remove_constants();
        }
      }
      return changed;
    }

    void apply_changes(IRCode* code) {
      for (auto const& p : branch_replacements) {
        auto const& old_op = p.first;
        auto const& new_op = p.second;
        if (new_op->opcode() == OPCODE_NOP) {
          code->remove_opcode(old_op);
          delete new_op;
        } else {
          code->replace_branch(old_op, new_op);
        }
      }
      branch_replacements.clear();
    }

    bool propagate_insn(IRInstruction *inst) {
      bool changed = false;
      switch (inst->opcode()) {
      case OPCODE_IF_EQ:
      case OPCODE_IF_NE:
      case OPCODE_IF_LT:
      case OPCODE_IF_GE:
      case OPCODE_IF_GT:
      case OPCODE_IF_LE:
      case OPCODE_IF_LTZ:
      case OPCODE_IF_GEZ:
      case OPCODE_IF_GTZ:
      case OPCODE_IF_LEZ:
      case OPCODE_IF_EQZ:
      case OPCODE_IF_NEZ: {
        IRInstruction* branch_replacement = propagate_branch(inst);
        if (branch_replacement != nullptr) {
          TRACE(CONSTP, 2, "Changed conditional branch %s\n", SHOW(inst));
          branch_replacements.emplace_back(inst, branch_replacement);
          m_branch_propagated++;
          changed = true;
        }
        break;
      }

      case OPCODE_MOVE_FROM16:
      case OPCODE_MOVE_16:
      case OPCODE_MOVE_OBJECT_FROM16:
      case OPCODE_MOVE_OBJECT_16:
      case OPCODE_MOVE:
      case OPCODE_MOVE_OBJECT:
        reg_values.move(inst->src(0), inst->dest());
        break;

      case OPCODE_MOVE_WIDE:
      case OPCODE_MOVE_WIDE_FROM16:
      case OPCODE_MOVE_WIDE_16:
        reg_values.move_wide(inst->src(0), inst->dest());
        break;

      case OPCODE_CMPL_FLOAT:
      case OPCODE_CMPG_FLOAT:
        // Must be IEEE 754
        if (std::numeric_limits<float>::is_iec559) {
          compare<float, int32_t>(inst);
        } else {
          TRACE(
              CONSTP,
              1,
              "Warning: not propagating floats because IEEE 754 is not in use");
        }
        break;
      case OPCODE_CMPL_DOUBLE:
      case OPCODE_CMPG_DOUBLE:
        // Must be IEEE 754
        if (std::numeric_limits<double>::is_iec559) {
          compare<double, int64_t>(inst);
        } else {
          TRACE(CONSTP,
                1,
                "Warning: not propagating doubles because IEEE 754 is not in "
                "use");
        }
        break;
      case OPCODE_CMP_LONG:
        compare<int64_t, int64_t>(inst);
        break;

      default:
        if (inst->dest_is_wide()) {
          reg_values.mark_unknown_wide(inst->dest());
        } else if (inst->dests_size() > 0) {
          reg_values.mark_unknown(inst->dest());
        }
        break;
      }
      return changed;
    }

    // A generic template for all the CMP instructions.
    // If we know enough, put -1, 0, or 1 into the destination register.
    //
    // Stored is how the data is stored in the register (the size).
    //   Should be int32_t or int64_t
    // Operand is how the data is used.
    //   Should be float, double, or int64_t
    template<typename Operand, typename Stored>
    void compare(IRInstruction* inst) {
      DexOpcode op = inst->opcode();
      boost::optional<Stored> left =
          get_register<Stored>(reg_values, inst->src(0));
      boost::optional<Stored> right =
          get_register<Stored>(reg_values, inst->src(1));

      if (left && right) {
        int32_t result;
        Operand l_val = reinterpret_bits<Operand, Stored>(*left);
        Operand r_val = reinterpret_bits<Operand, Stored>(*right);
        if (is_compare_floating(op) &&
            (std::isnan(l_val) || std::isnan(r_val))) {
          if (is_less_than_bias(op)) {
            result = -1;
          } else {
            result = 1;
          }
        } else if (l_val > r_val) {
          result = 1;
        } else if (l_val == r_val) {
          result = 0;
        } else { // l_val < r_val
          result = -1;
        }
        reg_values.put(result, inst->dest());
        return;
      }
      mark_unknown<Stored>(reg_values, inst->dest());
    }

    static bool is_compare_floating(DexOpcode op) {
      return op == OPCODE_CMPG_DOUBLE ||
             op == OPCODE_CMPL_DOUBLE ||
             op == OPCODE_CMPG_FLOAT ||
             op == OPCODE_CMPL_FLOAT;
    }

    static bool is_less_than_bias(DexOpcode op) {
      return op == OPCODE_CMPL_DOUBLE ||
             op == OPCODE_CMPL_FLOAT;
    }

    template<typename Out, typename In>
    // reinterpret the long's bits as a double
    static Out reinterpret_bits(In in) {
      if (std::is_same<In, Out>::value) {
        return in;
      }
      static_assert(sizeof(In) == sizeof(Out), "types must be same size");
      return *reinterpret_cast<Out*>(&in);
    }

    // Propagate const instruction value to registers
    void propagate_constant(IRInstruction *inst) {
      switch (inst->opcode()) {
      case OPCODE_CONST:
      case OPCODE_CONST_HIGH16:
      case OPCODE_CONST_4:
      case OPCODE_CONST_16:
        reg_values.put(inst->literal(), inst->dest());
        break;
      case OPCODE_CONST_WIDE_16:
      case OPCODE_CONST_WIDE_32:
      case OPCODE_CONST_WIDE:
      case OPCODE_CONST_WIDE_HIGH16:
        reg_values.put_wide(inst->literal(), inst->dest());
        break;
      default:
        if (inst->dest_is_wide()) {
          reg_values.mark_unknown_wide(inst->dest());
        } else if (inst->dests_size() > 0) {
          reg_values.mark_unknown(inst->dest());
        }
      }
    }

    // Evaluate the guard expression of an if opcode.
    // pass 0 as the r_val for if-*Z opcodes
    bool eval_if(DexOpcode op, int32_t l_val, int32_t r_val) {
      switch (op) {
      case OPCODE_IF_EQ:
      case OPCODE_IF_EQZ:
        return l_val == r_val;
      case OPCODE_IF_NE:
      case OPCODE_IF_NEZ:
        return l_val != r_val;
      case OPCODE_IF_LT:
      case OPCODE_IF_LTZ:
        return l_val < r_val;
      case OPCODE_IF_GE:
      case OPCODE_IF_GEZ:
        return l_val >= r_val;
      case OPCODE_IF_GT:
      case OPCODE_IF_GTZ:
        return l_val > r_val;
      case OPCODE_IF_LE:
      case OPCODE_IF_LEZ:
        return l_val <= r_val;
      default:
        always_assert_log(false, "opcode %s must be an if", SHOW(op));
      }
    }

    // Attempt to create replacements for branch instructions. Evaluate the
    // conditional based on known register values.
    //
    // returns a new GOTO if this branch is always true.
    // returns a new NOP if this branch is always false. Be sure to free it.
    // returns nullptr when this branch can't be removed
    //
    // precondition: inst must be a branch instruction (OPCODE_IF_*)
    IRInstruction* propagate_branch(IRInstruction *inst) {
      boost::optional<int32_t> left = reg_values.get(inst->src(0));
      if (!left) {
        return nullptr;
      }

      int32_t l_val = *left;
      int32_t r_val;
      if (inst->srcs_size() == 2) {
        boost::optional<int32_t> right = reg_values.get(inst->src(1));
        if (!right) {
          return nullptr;
        }
        r_val = *right;
      } else {
        // if-*Z vA        is the same as
        // if-*  vA, 0
        r_val = 0;
      }

      bool branch_result = eval_if(inst->opcode(), l_val, r_val);
      if (branch_result) {
        // Transform keeps track of the target and selects the right size
        // instruction based on the offset
        return new IRInstruction(OPCODE_GOTO);
      } else {
        return new IRInstruction(OPCODE_NOP);
      }
      return nullptr;
    }

    // This method calculates number of reachable predecessors of each block The
    // difference between this method and remove_unreachable_blocks in LocalDCE
    // is that this method only finds unreachable blocks without deleting any
    // edge or block
    void find_reachable_predecessors(
        const std::vector<Block*>& original_blocks,
        std::unordered_map<Block*, int>& block_to_predecessors_count) {
      std::vector<Block*> unreachable_blocks;
      for (size_t i = 1; i < original_blocks.size(); ++i) {
        auto pred_size = original_blocks[i]->preds().size();
        block_to_predecessors_count[original_blocks[i]] = pred_size;
        if (pred_size == 0) {
          unreachable_blocks.push_back(original_blocks[i]);
        }
      }
      while (unreachable_blocks.size() > 0) {
        auto b = unreachable_blocks.back();
        unreachable_blocks.pop_back();
        for (auto succs_b : b->succs()) {
          if (--block_to_predecessors_count[succs_b] == 0) {
            unreachable_blocks.push_back(succs_b);
          }
        }
      }
    }

  public:
    ConstantPropagation(
        const Scope& scope,
        const ConstantPropagationPassV2::Config& cfg)
      : m_scope(scope), m_config(cfg) {}

    void run() {
      walk_methods(m_scope,
        [&](DexMethod* m) {
          if (!m->get_code()) {
            return;
          }
          // Skipping blacklisted classes
          if (m_config.blacklist.count(m->get_class()) > 0) {
            TRACE(CONSTP, 2, "Skipping %s\n", show(m->get_class()).c_str());
            return;
          }
          propagate(m);
        });

      TRACE(CONSTP, 1,
        "Branch condition removed: %lu\n",
        m_branch_propagated);
    }

    size_t num_branch_propagated() const {
      return m_branch_propagated;
    }
  };
}

void ConstantPropagationPassV2::run_pass(DexStoresVector& stores,
                                       ConfigFiles& /* cfg */,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);
  ConstantPropagation constant_prop(scope, m_config);
  constant_prop.run();
  mgr.incr_metric(
    METRIC_BRANCH_PROPAGATED,
    constant_prop.num_branch_propagated());
}

static ConstantPropagationPassV2 s_pass;
