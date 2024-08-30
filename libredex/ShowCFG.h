/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRList.h"
#include "Show.h"

template <typename Special>
std::string show(const cfg::Block* block, Special& special) {
  std::ostringstream ss;
  for (const auto& mie : *block) {
    special.mie_before(ss, mie);
    ss << "   " << show(mie) << "\n";
    special.mie_after(ss, mie);
  }
  return ss.str();
}

template <typename Special>
std::string show(const cfg::ControlFlowGraph& cfg, Special& special) {
  const auto& blocks = cfg.blocks();
  std::ostringstream ss;
  ss << "CFG:\n";
  for (auto* b : blocks) {
    ss << " Block B" << b->id() << ":";
    if (b == cfg.entry_block()) {
      ss << " entry";
    }
    ss << "\n";
    special.start_block(ss, b);

    ss << "   preds:";
    for (const auto& p : b->preds()) {
      ss << " (" << *p << " B" << p->src()->id() << ")";
    }
    ss << "\n";

    ss << show(b, special);

    ss << "   succs:";
    for (auto& s : b->succs()) {
      ss << " (" << *s << " B" << s->target()->id() << ")";
    }
    ss << "\n";
    special.end_block(ss, b);
  }
  return ss.str();
}

// Special helper interjecting fixpoint iterator data.
namespace show_impl {

template <typename Environment, typename Iterator>
struct IteratorSpecial {
  Environment cur;
  const Iterator& iter;

  explicit IteratorSpecial(const Iterator& iter) : iter(iter) {}

  void mie_before(std::ostream& os, const MethodItemEntry& mie) {
    if (mie.type != MFLOW_OPCODE) {
      return;
    }
    os << "state: " << cur << "\n";
    iter.analyze_instruction(mie.insn, &cur);
  }
  void mie_after(std::ostream&, const MethodItemEntry&) {}

  void start_block(std::ostream& os, cfg::Block* b) {
    cur = iter.get_entry_state_at(b);
    os << "entry state: " << cur << "\n";
  }
  void end_block(std::ostream& os, cfg::Block* b) {
    const auto& exit_state = iter.get_exit_state_at(b);
    os << "exit state: " << exit_state << "\n";
  }
};

struct ArraysAndResIds {
  void mie_before(std::ostream&, const MethodItemEntry&) {}
  void mie_after(std::ostream& oss, const MethodItemEntry& mie) {
    const static std::string block_indent(3, ' ');
    if (mie.type == MFLOW_OPCODE) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_CONST && insn->get_literal() >= 0x7f000000 &&
          insn->get_literal() <= 0x7fffffff) {
        std::string indent;
        auto msg = show(mie);
        auto idx = msg.find(": ");
        if (idx != std::string::npos) {
          indent = std::string(idx + 2, ' ');
        }
        oss << block_indent << indent << "Resource ID: 0x" << std::hex
            << (uint32_t)insn->get_literal() << std::dec << std::endl;
      } else if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
        auto data = insn->get_data();
        oss << block_indent << "  " << SHOW(data) << std::endl;
      }
    }
  }
  void start_block(std::ostream&, const cfg::Block*) {}
  void end_block(std::ostream&, const cfg::Block*) {}
};
} // namespace show_impl

template <typename Environment, typename Iterator>
std::string show_analysis(const cfg::ControlFlowGraph& cfg,
                          const Iterator& iter) {
  show_impl::IteratorSpecial<Environment, Iterator> special(iter);
  return show(cfg, special);
}

inline std::string show_res_payloads(const cfg::ControlFlowGraph& cfg) {
  show_impl::ArraysAndResIds special;
  return show(cfg, special);
}
