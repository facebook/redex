/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockInstrument.h"

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "Inliner.h"
#include "LoopInfo.h"
#include "MethodReference.h"
#include "RedexContext.h"
#include "ScopedMetrics.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "TypeSystem.h"
#include "TypeUtil.h"
#include "Walkers.h"

#include <boost/algorithm/string/join.hpp>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace instrument;

namespace {

constexpr bool DEBUG_CFG = false;
constexpr size_t BIT_VECTOR_SIZE = 16;
constexpr int PROFILING_DATA_VERSION = 4;

using OnMethodExitMap =
    std::map<size_t, // arity of vector arguments (excluding `int offset`)
             DexMethod*>;

enum class BlockType {
  Unspecified = 0,
  Instrumentable = 1 << 0,
  Empty = 1 << 1,
  Useless = 1 << 2,
  Normal = 1 << 3,
  Catch = 1 << 4,
  MoveException = 1 << 5,
  NoSourceBlock = 1 << 6,
};

enum class InstrumentedType {
  // Too many basic blocks. We only did method tracing.
  MethodOnly = 1,
  Both = 2,
  // Rare cases: due to infinite loops, no onMethodExit was instrumented.
  UnableToTrackBlock = 3,
};

inline BlockType operator|(BlockType a, BlockType b) {
  return static_cast<BlockType>(static_cast<int>(a) | static_cast<int>(b));
}

inline BlockType operator&(BlockType a, BlockType b) {
  return static_cast<BlockType>(static_cast<int>(a) & static_cast<int>(b));
}

inline BlockType operator^(BlockType a, BlockType b) {
  return static_cast<BlockType>(static_cast<int>(a) ^ static_cast<int>(b));
}

std::ostream& operator<<(std::ostream& os, const BlockType& bt) {
  if (bt == BlockType::Unspecified) {
    os << "Unspecified";
    return os;
  }

  bool written{false};
  auto type = bt;

  if ((type & BlockType::Instrumentable) == BlockType::Instrumentable) {
    os << "Instrumentable";
    written = true;
    type = type ^ BlockType::Instrumentable;
  }

  if ((type & BlockType::Empty) == BlockType::Empty) {
    if (written) {
      os << ",";
    }
    os << "Empty";
    written = true;
    type = type ^ BlockType::Empty;
  }

  if ((type & BlockType::Useless) == BlockType::Useless) {
    if (written) {
      os << ",";
    }
    os << "Useless";
    written = true;
    type = type ^ BlockType::Useless;
  }

  if ((type & BlockType::Normal) == BlockType::Normal) {
    if (written) {
      os << ",";
    }
    os << "Normal";
    written = true;
    type = type ^ BlockType::Normal;
  }

  if ((type & BlockType::Catch) == BlockType::Catch) {
    if (written) {
      os << ",";
    }
    os << "Catch";
    written = true;
    type = type ^ BlockType::Catch;
  }

  if ((type & BlockType::MoveException) == BlockType::MoveException) {
    if (written) {
      os << ",";
    }
    os << "MoveException";
    written = true;
    type = type ^ BlockType::MoveException;
  }

  if ((type & BlockType::NoSourceBlock) == BlockType::NoSourceBlock) {
    if (written) {
      os << ",";
    }
    os << "NoSourceBlock";
    written = true;
    type = type ^ BlockType::NoSourceBlock;
  }

  if (type != BlockType::Unspecified) {
    if (written) {
      os << ",";
    }
    os << "Unknown";
  }

  return os;
}

std::string block_type_str(const BlockType& type) {
  std::ostringstream oss;
  oss << type;
  return oss.str();
}

using BitId = size_t;

struct BlockInfo {
  cfg::Block* block;
  loop_impl::Loop* loop;
  BlockType type;
  IRList::iterator it;
  BitId bit_id;
  size_t index_id;
  std::vector<cfg::Block*> merge_in;

  BlockInfo(cfg::Block* b,
            loop_impl::Loop* l,
            BlockType t,
            const IRList::iterator& i)
      : block(b),
        loop(l),
        type(t),
        it(i),
        bit_id(std::numeric_limits<BitId>::max()),
        index_id(std::numeric_limits<size_t>::max()) {}

  bool is_instrumentable() const {
    return (type & BlockType::Instrumentable) == BlockType::Instrumentable;
  }

  void update_merge(const BlockInfo& rhs) {
    block = rhs.block;
    type = rhs.type;
    it = rhs.it;
    bit_id = rhs.bit_id;
    index_id = rhs.index_id;
    merge_in.insert(merge_in.end(), rhs.merge_in.begin(), rhs.merge_in.end());
  }
};

struct MethodInfo {
  const DexMethod* method = nullptr;
  // All eligible methods are at least method instrumented. This indicates
  // whether this method is only method instrumented because of too many blocks.
  bool too_many_blocks = false;
  // The offset is used in `short[] DynamicAnalysis.sMethodStats`. The first two
  // shorts are for method method profiling, and short[num_vectors] are for
  // block coverages.
  size_t offset = 0;
  size_t hit_offset = 0;
  size_t num_non_entry_blocks = 0;
  size_t num_vectors = 0;
  size_t num_hit_blocks = 0;
  size_t num_loop_blocks = 0;
  size_t num_exit_calls = 0;

  size_t num_empty_blocks = 0;
  size_t num_useless_blocks = 0;
  size_t num_no_source_blocks = 0;
  size_t num_blocks_too_large = 0;
  size_t num_catches = 0;
  size_t num_instrumented_catches = 0;
  size_t num_instrumented_blocks = 0;
  size_t num_merged{0};
  size_t num_merged_not_instrumented{0};

  std::vector<cfg::BlockId> bit_id_2_block_id;
  std::vector<cfg::BlockId> hit_id_2_block_id;
  std::vector<std::vector<SourceBlock*>> bit_id_2_source_blocks;
  std::map<cfg::BlockId, BlockType> rejected_blocks;
  std::vector<SourceBlock*> entry_source_blocks;

  // For stats.
  size_t num_too_many_blocks{0};
  MethodInfo& operator+=(const MethodInfo& rhs) {
    num_non_entry_blocks += rhs.num_non_entry_blocks;
    num_vectors += rhs.num_vectors;
    num_hit_blocks += rhs.num_hit_blocks;
    num_loop_blocks += rhs.num_loop_blocks;
    num_exit_calls += rhs.num_exit_calls;
    num_empty_blocks += rhs.num_empty_blocks;
    num_useless_blocks += rhs.num_useless_blocks;
    num_no_source_blocks += rhs.num_no_source_blocks;
    num_blocks_too_large += rhs.num_blocks_too_large;
    num_catches += rhs.num_catches;
    num_instrumented_catches += rhs.num_instrumented_catches;
    num_instrumented_blocks += rhs.num_instrumented_blocks;
    num_merged += rhs.num_merged;
    num_merged_not_instrumented += rhs.num_merged_not_instrumented;
    num_too_many_blocks += rhs.num_too_many_blocks;

    return *this;
  }
};

InstrumentedType get_instrumented_type(const MethodInfo& i) {
  if (i.too_many_blocks) {
    return InstrumentedType::MethodOnly;
  } else if (i.num_exit_calls == 0 && i.num_vectors != 0) {
    return InstrumentedType::UnableToTrackBlock;
  } else {
    return InstrumentedType::Both;
  }
}

using MethodDictionary = std::unordered_map<const DexString*, size_t>;

MethodDictionary create_method_dictionary(
    const std::string& file_name, const std::vector<MethodInfo>& all_info) {
  std::unordered_set<const DexString*> methods_set;
  for (const auto& info : all_info) {
    methods_set.insert(info.method->get_deobfuscated_name_or_null());
    for (const auto& sb_vec : info.bit_id_2_source_blocks) {
      for (const auto* sb : sb_vec) {
        methods_set.insert(sb->src);
      }
    }
    for (const auto* sb : info.entry_source_blocks) {
      methods_set.insert(sb->src);
    }
  }
  std::vector<const DexString*> methods(methods_set.begin(), methods_set.end());
  std::sort(methods.begin(), methods.end(), compare_dexstrings);
  size_t idx{0};

  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  ofs << "type,version\nredex-source-block-method-dictionary,1\n";
  ofs << "index,deob_name\n";
  MethodDictionary method_dictionary;
  for (const auto* m : methods) {
    method_dictionary.emplace(m, idx);
    ofs << idx << "," << m->str() << "\n";
    ++idx;
  }

  return method_dictionary;
}

void write_metadata(const ConfigFiles& cfg,
                    const std::string& metadata_base_file_name,
                    const std::vector<MethodInfo>& all_info,
                    const std::string& strategy) {
  const auto method_dict = create_method_dictionary(
      cfg.metafile("redex-source-block-method-dictionary.csv"), all_info);

  // Write a short metadata of this metadata file in the first two lines.
  auto file_name = cfg.metafile(metadata_base_file_name);
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  ofs << "profile_type,version,num_methods" << std::endl;
  ofs << strategy << "," << PROFILING_DATA_VERSION << "," << all_info.size()
      << std::endl;

  // The real CSV-style metadata follows.
  if (strategy == "basic_block_hit_count") {
    const std::array<std::string, 11> headers = {
        "offset",           "name",        "instrument",
        "non_entry_blocks", "vectors",     "bit_id_2_block_id",
        "hit_offset",       "loop_blocks", "hit_id_2_block_id",
        "rejected_blocks",  "src_blocks"};
    ofs << boost::algorithm::join(headers, ",") << "\n";
  } else {
    const std::array<std::string, 8> headers = {
        "offset",           "name",      "instrument",
        "non_entry_blocks", "vectors",   "bit_id_2_block_id",
        "rejected_blocks",  "src_blocks"};
    ofs << boost::algorithm::join(headers, ",") << "\n";
  }

  auto write_block_id_map = [](const auto& bit_id_2_block_id) {
    std::vector<std::string> fields;
    fields.reserve(bit_id_2_block_id.size());
    for (cfg::BlockId id : bit_id_2_block_id) {
      fields.emplace_back(std::to_string(id));
    }
    return boost::algorithm::join(fields, ";");
  };

  auto rejected_blocks = [](const auto& rejected_blocks) {
    std::vector<std::string> fields;
    fields.reserve(rejected_blocks.size());
    for (const auto& p : rejected_blocks) {
      fields.emplace_back(std::to_string(p.first) + ":" +
                          std::to_string(static_cast<int>(p.second)));
    }
    return boost::algorithm::join(fields, ";");
  };

  auto to_hex = [](auto n) {
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(sizeof(n) * 2) << std::hex << n;
    return ss.str();
  };

  auto source_blocks = [&method_dict](const auto& entry_source_blocks,
                                      const auto& bit_id_2_source_blocks) {
    std::stringstream ss;
    bool first1 = true;
    auto handle_source_blocks = [&first1, &ss, &method_dict](const auto& v) {
      if (first1) {
        first1 = false;
      } else {
        ss << ";";
      }

      bool first2 = true;
      for (auto* sb : v) {
        // Do not list synthetic source blocks.
        if (sb->id == SourceBlock::kSyntheticId) {
          continue;
        }
        if (first2) {
          first2 = false;
        } else {
          ss << "|";
        }
        ss << method_dict.at(sb->src) << "#" << sb->id;
      }
    };

    // Entry block.
    handle_source_blocks(entry_source_blocks);
    for (const auto& v : bit_id_2_source_blocks) {
      handle_source_blocks(v);
    }

    return ss.str();
  };

  for (const auto& info : all_info) {

    if (strategy == "basic_block_hit_count") {
      const std::array<std::string, 11> fields = {
          std::to_string(info.offset),
          std::to_string(
              method_dict.at(info.method->get_deobfuscated_name_or_null())),
          std::to_string(static_cast<int>(get_instrumented_type(info))),
          std::to_string(info.num_non_entry_blocks),
          std::to_string(info.num_vectors),
          write_block_id_map(info.bit_id_2_block_id),
          std::to_string(info.hit_offset),
          std::to_string(info.num_hit_blocks),
          write_block_id_map(info.hit_id_2_block_id),
          rejected_blocks(info.rejected_blocks),
          source_blocks(info.entry_source_blocks, info.bit_id_2_source_blocks),
      };
      ofs << boost::algorithm::join(fields, ",") << "\n";
    } else {
      const std::array<std::string, 8> fields = {
          std::to_string(info.offset),
          std::to_string(
              method_dict.at(info.method->get_deobfuscated_name_or_null())),
          std::to_string(static_cast<int>(get_instrumented_type(info))),
          std::to_string(info.num_non_entry_blocks),
          std::to_string(info.num_vectors),
          write_block_id_map(info.bit_id_2_block_id),
          rejected_blocks(info.rejected_blocks),
          source_blocks(info.entry_source_blocks, info.bit_id_2_source_blocks),
      };
      ofs << boost::algorithm::join(fields, ",") << "\n";
    }
  }

  TRACE(INSTRUMENT, 2, "Metadata file was written to: %s", SHOW(file_name));
}

std::vector<cfg::Block*> only_terminal_return_or_throw_blocks(
    cfg::ControlFlowGraph& cfg) {

  // For example, `real_exit_blocks` returns the following 4 exit blocks. But we
  // don't need to instrument exit blocks that are still with successors.
  //
  // Block B22: <== exit block
  //   preds: (goto B20)
  //   OPCODE: MONITOR_EXIT v3
  //   succs: (goto B23) (throw B42)
  // Block B23: <== exit block to be instrumented
  //   preds: (goto B22)
  //   OPCODE: RETURN_VOID
  //   succs:
  // ...
  // Block B42: <== exit block
  //   preds: (throw B4) (throw B2) (throw B20) (throw B19) ..
  //   OPCODE: MOVE_EXCEPTION v9
  //   OPCODE: MONITOR_EXIT v3
  //   succs: (throw B42) (goto B44)
  // Block B44: <== exit block to be instrumented
  //   preds: (goto B42)
  //   [0x7f3b1745c440] OPCODE: THROW v9
  //   succs:
  //
  // And note that as of now, we don't consider infinite loop only methods.
  std::vector<cfg::Block*> blocks = cfg.real_exit_blocks(false);

  // So, we extract really real exit blocks without any successors.
  blocks.erase(
      std::remove_if(blocks.begin(), blocks.end(),
                     [](const auto& b) { return !b->succs().empty(); }),
      blocks.end());
  return blocks;
}

IRList::iterator get_first_non_move_result_insn(cfg::Block* b) {
  for (auto it = b->begin(); it != b->end(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    if (!opcode::is_move_result_any(it->insn->opcode())) {
      return it;
    }
  }
  return b->end();
}

IRList::iterator get_first_next_of_move_except(cfg::Block* b) {
  IRList::iterator insert_pos = std::next(b->get_first_insn());
  while (insert_pos != b->end() && insert_pos->type != MFLOW_OPCODE) {
    insert_pos = std::next(insert_pos);
  }
  return insert_pos;
}

OnMethodExitMap build_onMethodExit_map(const DexClass& cls,
                                       const std::string& onMethodExit_name,
                                       const DexType* return_type) {
  OnMethodExitMap onMethodExit_map;
  for (const auto& m : cls.get_dmethods()) {
    const auto name = m->get_name()->str();
    if (onMethodExit_name != name) {
      continue;
    }

    // The prototype of onMethodExit must be one of:
    // - onMethodExit(int offset), or
    // - onMethodExit(int offset, short vec1, ..., short vecN);
    const auto* args = m->get_proto()->get_args();
    always_assert_log(!args->empty(), "%s", SHOW(m));
    always_assert_log(*args->begin() == DexType::make_type("I"), "%s", SHOW(m));
    always_assert_log(std::none_of(std::next(args->begin(), 1), args->end(),
                                   [](const auto& type) {
                                     return type != DexType::make_type("S");
                                   }),
                      "%s", SHOW(m));
    always_assert_log(
        m->get_proto()->get_rtype() == return_type,
        "Analysis method %s does not have expected return type %s", SHOW(m),
        SHOW(return_type));

    // -1 is to exclude `int offset`.
    onMethodExit_map[args->size() - 1] = m;
  }

  always_assert_log(!onMethodExit_map.empty(),
                    "[InstrumentPass] error: cannot find %s in %s:\n%s",
                    onMethodExit_name.c_str(), SHOW(cls),
                    [&]() {
                      std::stringstream ss;
                      for (const auto& m : cls.get_dmethods()) {
                        ss << " " << show(m) << std::endl;
                      }
                      return ss.str();
                    }()
                        .c_str());

  return onMethodExit_map;
}

DexMethod* load_onMethodBegin(const DexClass& cls,
                              const std::string& method_name) {
  for (const auto& m : cls.get_dmethods()) {
    const auto name = m->get_name()->str();
    if (method_name != name) {
      continue;
    }
    const auto* args = m->get_proto()->get_args();
    if (args->size() != 1 || *args->begin() != DexType::make_type("I")) {
      always_assert_log(
          false,
          "[InstrumentPass] error: Proto type of onMethodBegin must be "
          "onMethodBegin(int), but it was %s",
          show(m->get_proto()).c_str());
    }
    return m;
  }

  std::stringstream ss;
  for (const auto& m : cls.get_dmethods()) {
    ss << " " << show(m) << std::endl;
  }
  always_assert_log(false, "[InstrumentPass] error: cannot find %s in %s:\n%s",
                    method_name.c_str(), SHOW(cls), ss.str().c_str());
}

auto insert_prologue_insts(cfg::ControlFlowGraph& cfg,
                           DexMethod* onMethodBegin,
                           const size_t num_vectors,
                           const size_t method_offset,
                           const size_t hit_offset,
                           std::vector<BlockInfo>& blocks) {
  std::vector<reg_t> reg_vectors(num_vectors);
  std::vector<IRInstruction*> prologues(num_vectors + 3);

  // Create instructions to allocate a set of 16-bit bit vectors.
  for (size_t i = 0; i < num_vectors; ++i) {
    prologues.at(i) = new IRInstruction(OPCODE_CONST);
    prologues.at(i)->set_literal(0);
    reg_vectors.at(i) = cfg.allocate_temp();
    prologues.at(i)->set_dest(reg_vectors.at(i));
  }

  // Do onMethodBegin instrumentation. We allocate a register that holds the
  // method offset, which is used for all onMethodBegin/Exit.
  IRInstruction* method_offset_inst = new IRInstruction(OPCODE_CONST);
  method_offset_inst->set_literal(method_offset);
  const reg_t reg_method_offset = cfg.allocate_temp();
  method_offset_inst->set_dest(reg_method_offset);
  prologues.at(num_vectors) = method_offset_inst;

  IRInstruction* hit_offset_inst = new IRInstruction(OPCODE_CONST);
  hit_offset_inst->set_literal(hit_offset);
  const reg_t reg_hit_offset = cfg.allocate_temp();
  hit_offset_inst->set_dest(reg_hit_offset);
  prologues.at(num_vectors + 1) = hit_offset_inst;

  IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_inst->set_method(onMethodBegin);
  invoke_inst->set_srcs_size(1);
  invoke_inst->set_src(0, reg_method_offset);
  prologues.at(num_vectors + 2) = invoke_inst;

  auto eb = cfg.entry_block();
  IRInstruction* eb_insn = nullptr;
  if (!blocks.empty()) {
    auto& b = blocks.front();
    if (b.block == eb && b.it != b.block->end()) {
      eb_insn = b.it->insn;
    }
  }

  // Insert all prologue opcodes to the entry block (right after param loading).
  cfg.entry_block()->insert_before(
      cfg.entry_block()->to_cfg_instruction_iterator(
          cfg.entry_block()->get_first_non_param_loading_insn()),
      prologues);

  // If the entry block was to be instrumented, the iterator is invalid now.
  if (eb_insn != nullptr) {
    auto& b = blocks.front();
    auto it = cfg.find_insn(eb_insn);
    b.block = it.block();
    b.it = it.unwrap();
  }

  if (slow_invariants_debug) {
    for (const auto& b : blocks) {
      if (b.block == eb) {
        continue;
      }
      auto is_in_block = [&]() {
        for (auto it = b.block->begin(); it != b.block->end(); ++it) {
          if (it == b.it) {
            return true;
          }
        }
        return false;
      };
      assert_log(b.block->end() == b.it || is_in_block(), "%s\n%s",
                 SHOW(b.block), SHOW(*b.it));
    }
  }

  return std::make_tuple(reg_vectors, reg_method_offset, reg_hit_offset);
}

std::tuple<size_t, std::vector<IRInstruction*>> insert_onMethodExit_calls(
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    const std::vector<reg_t>& reg_vectors, // May be empty
    const size_t method_offset,
    const reg_t reg_method_offset,
    const std::map<size_t, DexMethod*>& onMethodExit_map,
    const std::map<size_t, DexMethod*>& onMethodExitUnchecked_map,
    const size_t max_vector_arity,
    const std::vector<short>& loop_shorts,
    const InstrumentPass::Options& options) {
  std::vector<IRInstruction*> invokes;
  size_t num_exit_blocks = 0;
  // If reg_vectors is emptry (methods with a single entry block), no need to
  // instrument onMethodExit.
  if (reg_vectors.empty()) {
    return std::make_tuple(num_exit_blocks, invokes);
  }

  struct SingletonTempReg {
    cfg::ControlFlowGraph& cfg;
    std::optional<reg_t> reg = std::nullopt;
    bool wide;
    SingletonTempReg(cfg::ControlFlowGraph& cfg, bool wide)
        : cfg(cfg), wide(wide) {}

    reg_t operator*() {
      if (!reg) {
        reg = wide ? cfg.allocate_wide_temp() : cfg.allocate_temp();
      }
      return *reg;
    }
    operator bool() const { return reg.has_value(); }
  };

  // When a method exits, we call onMethodExit to pass all vectors to record.
  // onMethodExit is overloaded to some degrees (e.g., up to 5 vectors). If
  // number of vectors > 5, generate one or more onMethodExit calls.
  const size_t num_vectors = reg_vectors.size();
  const size_t num_invokes =
      std::max(1., std::ceil(double(num_vectors) / double(max_vector_arity)));

  auto inject_onMethodExit = [&](cfg::Block* block,
                                 const cfg::InstructionIterator& before_it,
                                 SingletonTempReg& invoke_result_tmp_reg) {
    // Standard layout:
    //
    // if (onMethodExit(0, ...)) {
    //   onMethodExitUnchecked(1, ...)
    //   onMethodExitUnchecked(2, ...)
    //   ...
    // }

    size_t offset = method_offset;
    size_t v = num_vectors;
    size_t iteration = 0;

    auto create_call = [&](const auto& method_map) {
      const size_t arity = std::min(v, max_vector_arity);

      IRInstruction* inst = (new IRInstruction(OPCODE_INVOKE_STATIC))
                                ->set_method(method_map.at(arity))
                                ->set_srcs_size(arity + 1)
                                ->set_src(0, reg_method_offset);
      for (size_t j = 0; j < arity; ++j) {
        inst->set_src(j + 1, reg_vectors[max_vector_arity * iteration + j]);
      }

      iteration++;
      v -= arity;

      return inst;
    };

    auto checked_invoke = create_call(onMethodExit_map);
    if (v == 0) {
      // No tail of unchecked calls, don't create control flow.
      block->insert_before(before_it, checked_invoke);
      return block;
    }

    auto head_block = cfg.split_block_before(block, before_it.unwrap());
    // Assumption: block has no instructions before before_it, so that
    // head_block is empty.
    redex_assert(head_block->num_opcodes() == 0);
    head_block->push_back(checked_invoke);
    head_block->push_back((new IRInstruction(OPCODE_MOVE_RESULT))
                              ->set_dest(*invoke_result_tmp_reg));
    auto cmp_insn =
        (new IRInstruction(OPCODE_IF_NEZ))->set_src(0, *invoke_result_tmp_reg);

    // Create a new block with the unchecked calls.
    auto unchecked_block = cfg.create_block();
    while (v != 0) {
      // Move forward the offset.
      offset += max_vector_arity;

      unchecked_block->push_back((new IRInstruction(OPCODE_CONST))
                                     ->set_literal(offset)
                                     ->set_dest(reg_method_offset));

      unchecked_block->push_back(create_call(onMethodExitUnchecked_map));
    }

    cfg.create_branch(head_block, cmp_insn, block, unchecked_block);

    cfg.add_edge(unchecked_block, block, cfg::EDGE_GOTO);

    return head_block;
  };

  auto create_invoke_insts_hit = [&]() -> auto {
    // This code works in case of num_invokes == 1.
    std::vector<IRInstruction*> invoke_insts((num_invokes * 2 - 1) +
                                             num_vectors);
    std::vector<IRInstruction*> invoke_inline(num_invokes);
    for (size_t j = 0; j < num_vectors; ++j) {
      const reg_t& reg = reg_vectors[j];
      short vec = loop_shorts[j];
      short inv_vec = ~vec;
      IRInstruction* inst_and = new IRInstruction(OPCODE_AND_INT_LIT);
      TRACE(INSTRUMENT, 8,
            "Normal Vector for Just Loop Blocks (%hu) inverted (%hu)", vec,
            inv_vec);
      inst_and->set_literal(inv_vec);
      inst_and->set_src(0, reg);
      inst_and->set_dest(reg);
      invoke_insts.at(j) = inst_and;
    }

    size_t offset = method_offset;
    for (size_t i = 0, v = num_vectors; i < num_invokes;
         ++i, v -= max_vector_arity) {
      const size_t arity = std::min(v, max_vector_arity);

      IRInstruction* inst = new IRInstruction(OPCODE_INVOKE_STATIC);
      inst->set_method(onMethodExit_map.at(arity));
      inst->set_srcs_size(arity + 1);
      inst->set_src(0, reg_method_offset);
      for (size_t j = 0; j < arity; ++j) {
        inst->set_src(j + 1, reg_vectors[max_vector_arity * i + j]);
      }
      invoke_insts.at(num_vectors + (i * 2)) = inst;
      invoke_inline.at(i) = inst;

      if (i != num_invokes - 1) {
        inst = new IRInstruction(OPCODE_CONST);
        // Move forward the offset.
        offset += max_vector_arity;
        inst->set_literal(offset);
        inst->set_dest(reg_method_offset);
        invoke_insts.at(num_vectors + (i * 2 + 1)) = inst;
      }
    }
    return std::make_tuple(invoke_insts, invoke_inline);
  };

  // Which blocks should have onMethodExits? Let's ignore infinite loop cases,
  // and do on returns/throws that have no successors.

  // Deduping these blocks can help. But it turns out it is too restricted
  // because it is sensitive to registers. As such, we do this manually.
  //
  // Because of catch handlers this is more complicated than it should be. We
  // do need duplicates to retain the right throw edges.
  //
  // For simplicity we will always rename the throw/return-non-void register.
  // That is easier than remembering and fixing it up later, and reg-alloc
  // should be able to deal with it.
  using CatchCoverage =
      std::vector<std::pair<const DexType*, const cfg::Block*>>;
  auto create_catch_coverage = [](const cfg::Block* b) {
    auto index_order = b->get_outgoing_throws_in_order();
    CatchCoverage ret{};
    ret.reserve(index_order.size());
    std::transform(index_order.begin(), index_order.end(),
                   std::back_inserter(ret), [](auto e) {
                     return std::pair<const DexType*, const cfg::Block*>(
                         e->throw_info()->catch_type, e->target());
                   });
    return ret;
  };
  struct CatchCoverageHash {
    std::size_t operator()(const CatchCoverage& key) const {
      std::size_t seed = 0;
      boost::hash_range(seed, key.begin(), key.end());
      return seed;
    }
  };
  using DedupeMap =
      std::unordered_map<CatchCoverage, cfg::Block*, CatchCoverageHash>;

  enum RegType {
    kNone,
    kObject,
    kInt,
    kWide,
  };

  SingletonTempReg invoke_result_tmp_reg(cfg, /*wide=*/false);

  auto handle_instrumentation = [&cfg, &inject_onMethodExit,
                                 &create_invoke_insts_hit, &invokes, &options,
                                 &invoke_result_tmp_reg](
                                    DedupeMap& map, SingletonTempReg& tmp_reg,
                                    cfg::Block* b, CatchCoverage& cv,
                                    RegType reg_type) {
    auto pushback_move = [reg_type](cfg::Block* b, reg_t from, reg_t to) {
      auto move_insn =
          new IRInstruction(reg_type == RegType::kObject ? OPCODE_MOVE_OBJECT
                            : reg_type == RegType::kWide ? OPCODE_MOVE_WIDE
                                                         : OPCODE_MOVE);
      move_insn->set_src(0, from);
      move_insn->set_dest(to);
      b->push_back(move_insn);
    };

    auto it = map.find(cv);
    if (it == map.end()) {
      // Split before the last instruction.
      auto new_pred = cfg.split_block_before(b, b->get_last_insn());

      auto last_insn = b->get_last_insn()->insn;

      // If there is a reg involved, check for a temp reg, rename the
      // operand operand, and insert a move.
      if (reg_type != RegType::kNone) {
        // Insert a move.
        pushback_move(new_pred, last_insn->src(0), *tmp_reg);
        // Change the return's operand.
        last_insn->set_src(0, *tmp_reg);
      }

      // Now instrument the return.

      if (options.instrumentation_strategy == "basic_block_hit_count") {
        // TODO: Single-check code.
        std::vector<IRInstruction*> hit_count_insts;
        std::tie(hit_count_insts, invokes) = create_invoke_insts_hit();
        b->insert_before(b->to_cfg_instruction_iterator(b->get_last_insn()),
                         hit_count_insts);
      } else {
        b = inject_onMethodExit(
            b,
            b->to_cfg_instruction_iterator(b->get_last_insn()),
            invoke_result_tmp_reg);
      }

      // And store in the cache.
      map.emplace(std::move(cv), b);
    } else {
      auto last_insn = b->get_last_insn()->insn;
      std::optional<reg_t> ret_reg =
          reg_type == RegType::kNone ? std::nullopt
                                     : std::optional<reg_t>(last_insn->src(0));
      // Delete the last instruction, possibly add an aligning move, then
      // fall-through.
      b->remove_insn(b->get_last_insn());
      if (ret_reg) {
        redex_assert(tmp_reg);
        pushback_move(b, *ret_reg, *tmp_reg);
      }
      cfg.add_edge(b, it->second, cfg::EdgeType::EDGE_GOTO);
    }
  };

  DedupeMap return_map{};
  DedupeMap throw_map{};
  SingletonTempReg return_temp_reg{
      cfg, type::is_wide_type(method->get_proto()->get_rtype())};
  SingletonTempReg throw_temp_reg{cfg, /*wide=*/false};

  const auto& exit_blocks = only_terminal_return_or_throw_blocks(cfg);
  for (cfg::Block* b : exit_blocks) {
    assert(b->succs().empty());

    auto cv = create_catch_coverage(b);

    if (b->branchingness() == opcode::Branchingness::BRANCH_RETURN) {
      auto ret_insn = b->get_last_insn()->insn;
      auto ret_opcode = ret_insn->opcode();
      redex_assert(opcode::is_a_return(ret_opcode));
      handle_instrumentation(
          return_map, return_temp_reg, b, cv,
          opcode::is_return_void(ret_opcode)     ? RegType::kNone
          : opcode::is_return_object(ret_opcode) ? RegType::kObject
          : opcode::is_return_wide(ret_opcode)   ? RegType::kWide
                                                 : RegType::kInt);
      redex_assert(return_temp_reg || opcode::is_return_void(ret_opcode));
    } else {
      redex_assert(b->branchingness() == opcode::Branchingness::BRANCH_THROW);
      handle_instrumentation(throw_map, throw_temp_reg, b, cv,
                             RegType::kObject);
    }
  }
  return std::make_tuple(exit_blocks.size(), invokes);
}

// Very simplistic setup: if we think we can elide putting instrumentation into
// a block by pushing the source blocks into the next, we will do it - under the
// strong assumption that two "empty/useless" blocks do not usually follow each
// other.
void create_block_info(
    const DexMethod* method,
    cfg::Block* block,
    const InstrumentPass::Options& options,
    const std::unordered_map<const cfg::Block*, BlockInfo*>& block_mapping) {
  auto* trg_block_info = block_mapping.at(block);

  auto trace_at_exit = at_scope_exit([&]() {
    TRACE(INSTRUMENT, 9, "Checking block B%zu for %s: %x=%s\n%s", block->id(),
          SHOW(method), (uint32_t)trg_block_info->type,
          block_type_str(trg_block_info->type).c_str(), SHOW(block));
  });

  // `Block.num_opcodes` skips internal opcodes, but we need the source
  // blocks.
  auto has_opcodes = [&]() {
    for (auto& mie : *block) {
      if (mie.type == MFLOW_OPCODE) {
        return true;
      }
    }
    return false;
  }();

  // See if this is a simple chain. For that the current block must have only
  // one out edge of type GOTO, and the target must have only when in edge.
  // Otherwise pushing the source blocks over would lose precision.
  auto single_next_ok = [&]() -> std::optional<const cfg::Block*> {
    // Find the target block, if any.
    const auto& succs = block->succs();
    if (succs.empty()) {
      return std::nullopt;
    }

    // Check forward direction.
    if (succs.size() != 1 || succs[0]->type() != cfg::EDGE_GOTO ||
        succs[0]->target() == block->cfg().entry_block() ||
        succs[0]->target() == block) {
      return std::nullopt;
    }

    const auto* trg_block = succs[0]->target();
    const auto& preds = trg_block->preds();
    redex_assert(!preds.empty());
    if (preds.size() != 1) {
      return std::nullopt;
    }
    // Really assume the integrity of the CFG here...

    return trg_block;
  };

  if (!has_opcodes) {
    if (!source_blocks::has_source_blocks(block)) {
      trg_block_info->update_merge(
          {block, trg_block_info->loop, BlockType::Empty, {}});
      return;
    }

    TRACE(INSTRUMENT, 9, "%s Block B%zu has no opcodes but source blocks!\n%s",
          SHOW(method), block->id(), SHOW(block->cfg()));
    // Find the target block, if any.
    if (auto next_opt = single_next_ok()) {
      // OK, we can virtually merge the source blocks into the following one.
      TRACE(INSTRUMENT, 9, "Not instrumenting empty block B%zu", block->id());
      block_mapping.at(*next_opt)->merge_in.push_back(block);
      trg_block_info->update_merge(
          {block, trg_block_info->loop, BlockType::Empty, {}});
      return;
    }
  }

  // TODO: There is a potential register allocation issue when we instrument
  // extremely large number of basic blocks. We've found a case. So, for now,
  // we don't instrument catch blocks with the hope these blocks are cold.
  if (block->is_catch() && !options.instrument_catches) {
    trg_block_info->update_merge(
        {block, trg_block_info->loop, BlockType::Catch, {}});
    return;
  }

  IRList::iterator insert_pos;
  BlockType type = block->is_catch() ? BlockType::Catch : BlockType::Normal;
  if (block->starts_with_move_result()) {
    insert_pos = get_first_non_move_result_insn(block);
  } else if (block->starts_with_move_exception()) {
    // move-exception must only ever occur as the first instruction of an
    // exception handler; anywhere else is invalid. So, take the next
    // instruction of the move-exception.
    insert_pos = get_first_next_of_move_except(block);
    type = type | BlockType::MoveException;
  } else {
    insert_pos = block->get_first_non_param_loading_insn();
  }

  if (insert_pos == block->end()) {
    if (source_blocks::has_source_blocks(block)) {
      if (auto next_opt = single_next_ok()) {
        // OK, we can virtually merge the source blocks into the following one.
        TRACE(INSTRUMENT, 9, "Not instrumenting useless block B%zu\n%s",
              block->id(), SHOW(block));
        block_mapping.at(*next_opt)->merge_in.push_back(block);
        trg_block_info->update_merge(
            {block, trg_block_info->loop, BlockType::Useless, {}});
        return;
      }
    }
  }

  // No source block? Then we can't map back block coverage data to source
  // block. No need to instrument unless this block is exit block (no succs).
  // Exit blocks will have onMethodEnd. We still need to instrument anyhow.
  if (!options.instrument_blocks_without_source_block &&
      !source_blocks::has_source_blocks(block) && !block->succs().empty()) {
    trg_block_info->update_merge(
        {block, trg_block_info->loop, BlockType::NoSourceBlock | type, {}});
    return;
  }

  trg_block_info->update_merge({block, trg_block_info->loop,
                                BlockType::Instrumentable | type, insert_pos});
}

auto get_blocks_to_instrument(const DexMethod* m,
                              const cfg::ControlFlowGraph& cfg,
                              const size_t max_num_blocks,
                              const InstrumentPass::Options& options) {
  // Collect basic blocks in the order of the source blocks (DFS).
  std::vector<cfg::Block*> blocks;

  auto block_start_fn = [&](cfg::Block* b) {
    // We don't instrument entry block.
    //
    // But there's an exceptional case. If the entry block is in a try-catch
    // (which actually happens very rarely), inserting onMethodBegin will create
    // an additional block because onMethodBegin may throw. The original entry
    // block becomes non-entry. In this case, we still instrument the entry
    // block at this moment. See testFunc10 in InstrumentBasicBlockTarget.java.
    //
    // So, don't add entry block if it is not in any try-catch.
    if (cfg.entry_block() == b &&
        cfg.entry_block()->get_outgoing_throws_in_order().empty()) {
      return;
    }
    blocks.push_back(b);
  };
  source_blocks::impl::visit_in_order(
      &cfg, block_start_fn, [](cfg::Block*, const cfg::Edge*) {},
      [](cfg::Block*) {});

  loop_impl::LoopInfo LI(cfg);

  // Future work: Pick minimal instrumentation candidates.
  std::vector<BlockInfo> block_info_list;
  block_info_list.reserve(blocks.size());
  std::unordered_map<const cfg::Block*, BlockInfo*> block_mapping;
  for (cfg::Block* b : blocks) {
    block_info_list.emplace_back(b, LI.get_loop_for(b), BlockType::Unspecified,
                                 b->end());
    block_mapping[b] = &block_info_list.back();
  }

  BitId id = 0;
  size_t hit_id = 0;
  for (cfg::Block* b : blocks) {
    create_block_info(m, b, options, block_mapping);
    auto* info = block_mapping[b];
    if ((info->type & BlockType::Instrumentable) == BlockType::Instrumentable) {
      if (id >= max_num_blocks) {
        // This is effectively rejecting all blocks.
        return std::make_tuple(std::vector<BlockInfo>{}, BitId(0), size_t(0),
                               true /* too many block */);
      }

      info->index_id = hit_id++;
      info->bit_id = id++;
    }
  }
  redex_assert(std::all_of(
      block_info_list.begin(), block_info_list.end(),
      [](const auto& bi) { return bi.type != BlockType::Unspecified; }));

  return std::make_tuple(block_info_list, id, hit_id, false);
}

void insert_block_coverage_computations(const std::vector<BlockInfo>& blocks,
                                        const std::vector<reg_t>& reg_vectors) {
  for (const auto& info : blocks) {
    if (!info.is_instrumentable()) {
      continue;
    }

    const BitId bit_id = info.bit_id;
    const size_t vector_id = bit_id / BIT_VECTOR_SIZE;
    cfg::Block* block = info.block;
    const auto& insert_pos = info.it;

    // bit_vectors[vector_id] |= 1 << bit_id'
    IRInstruction* inst = new IRInstruction(OPCODE_OR_INT_LIT);
    inst->set_literal(static_cast<int16_t>(1ULL << (bit_id % BIT_VECTOR_SIZE)));
    inst->set_src(0, reg_vectors.at(vector_id));
    inst->set_dest(reg_vectors.at(vector_id));
    block->insert_before(block->to_cfg_instruction_iterator(insert_pos), inst);
  }
}

std::vector<IRInstruction*> insert_hit_count_insts(
    cfg::ControlFlowGraph& cfg,
    DexMethod* onBlockHit,
    const size_t hit_offset,
    std::vector<BlockInfo>& blocks,
    size_t& num_loop_blocks) {
  const reg_t reg_hit_offset = cfg.allocate_temp();

  std::vector<IRInstruction*> invokes(num_loop_blocks);
  size_t index = 0;

  for (const auto& info : blocks) {
    if (!info.is_instrumentable() || info.loop == nullptr) {
      continue;
    }

    const size_t index_id = info.index_id;
    cfg::Block* prev = info.block;
    const size_t offset = index_id + hit_offset;

    cfg::Block* succ =
        cfg.split_block(prev, prev->get_first_non_param_loading_insn());

    cfg::Block* block = cfg.create_block();
    cfg.insert_block(prev, succ, block);
    const auto& insert_pos = block->end();

    // Do onBlockHit instrumentation. We allocate a register that holds the
    // hit offset, which is used for all onBlockHit.
    IRInstruction* hit_offset_inst = new IRInstruction(OPCODE_CONST);
    hit_offset_inst->set_literal(offset);
    hit_offset_inst->set_dest(reg_hit_offset);
    block->insert_before(block->to_cfg_instruction_iterator(insert_pos),
                         hit_offset_inst);

    IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
    invoke_inst->set_method(onBlockHit);
    invoke_inst->set_srcs_size(1);
    invoke_inst->set_src(0, reg_hit_offset);
    invokes.at(index) = invoke_inst;
    block->insert_before(block->to_cfg_instruction_iterator(insert_pos),
                         invoke_inst);
    index += 1;
  }

  return invokes;
}

MethodInfo instrument_basic_blocks(
    IRCode& code,
    DexMethod* method,
    DexMethod* onMethodBegin,
    const OnMethodExitMap& onMethodExit_map,
    const OnMethodExitMap& onMethodExitUnchecked_map,
    DexMethod* onBlockHit,
    const OnMethodExitMap& onNonLoopBlockHit_map,
    const size_t max_vector_arity,
    const size_t max_vector_arity_hit,
    const size_t method_offset,
    const size_t hit_offset,
    const size_t max_num_blocks,
    MultiMethodInliner& inliner,
    const InstrumentPass::Options& options) {
  MethodInfo info;
  info.method = method;

  using namespace cfg;

  code.build_cfg(/*editable*/ true);
  ControlFlowGraph& cfg = code.cfg();

  std::string before_cfg =
      traceEnabled(INSTRUMENT, 7) ? show(cfg) : std::string("");

  // Step 1: Get sorted basic blocks to instrument with their information.
  //
  // The blocks are sorted in RPO. We don't instrument entry blocks. If too many
  // blocks, it falls back to empty blocks, which is method tracing.
  std::vector<BlockInfo> blocks;
  size_t num_to_instrument;
  size_t num_instrument_hit_blocks;
  size_t num_instrument_loop_blocks = 0;
  bool too_many_blocks;
  std::tie(blocks, num_to_instrument, num_instrument_hit_blocks,
           too_many_blocks) =
      get_blocks_to_instrument(method, cfg, max_num_blocks, options);

  TRACE(INSTRUMENT, DEBUG_CFG ? 0 : 10, "BEFORE: %s, %s\n%s",
        show_deobfuscated(method).c_str(), SHOW(method), SHOW(cfg));

  // Step 2: Fill in some info eagerly. This is necessary as later steps may be
  //         modifying the CFG.
  info.bit_id_2_block_id.reserve(num_to_instrument);
  info.bit_id_2_source_blocks.reserve(num_to_instrument);
  info.hit_id_2_block_id.reserve(num_instrument_hit_blocks);
  for (const auto& i : blocks) {
    if (i.is_instrumentable()) {
      info.bit_id_2_block_id.push_back(i.block->id());
      info.hit_id_2_block_id.push_back(i.block->id());
      info.bit_id_2_source_blocks.emplace_back(
          source_blocks::gather_source_blocks(i.block));
      if (i.loop != nullptr) {
        num_instrument_loop_blocks += 1;
      }
      for (auto* merged_block : i.merge_in) {
        auto& vec = info.bit_id_2_source_blocks.back();
        auto sb_vec = source_blocks::gather_source_blocks(merged_block);
        vec.insert(vec.end(), sb_vec.begin(), sb_vec.end());
      }
      TRACE(INSTRUMENT, 10, "%s Block %zu: idx=%zu SBs=%s",
            show_deobfuscated(method).c_str(), i.block->id(),
            info.bit_id_2_block_id.size() - 1,
            [&]() {
              std::string ret;
              for (auto* sb : info.bit_id_2_source_blocks.back()) {
                ret.append(sb->show());
                ret.append(";");
              }
              return ret;
            }()
                .c_str());
    } else {
      info.rejected_blocks[i.block->id()] = i.type;
    }
  }

  // Step 3: Insert onMethodBegin to track method execution, and bit-vector
  //         allocation code in its method entry point.
  //
  const size_t origin_num_non_entry_blocks = cfg.num_blocks() - 1;
  const size_t num_vectors =
      std::ceil(num_to_instrument / double(BIT_VECTOR_SIZE));

  std::vector<reg_t> reg_vectors;
  std::vector<short> loop_shorts(num_vectors);
  reg_t reg_method_offset;
  reg_t reg_hit_offset;
  std::tie(reg_vectors, reg_method_offset, reg_hit_offset) =
      insert_prologue_insts(cfg, onMethodBegin, num_vectors, method_offset,
                            hit_offset, blocks);
  const size_t after_prologue_num_non_entry_blocks = cfg.num_blocks() - 1;

  // Step 4: Insert block coverage update instructions to each blocks.
  //
  insert_block_coverage_computations(blocks, reg_vectors);

  TRACE(INSTRUMENT, DEBUG_CFG ? 0 : 10, "WITH COVERAGE INSNS: %s, %s\n%s",
        show_deobfuscated(method).c_str(), SHOW(method), SHOW(cfg));

  // Gather early as step 4 may modify CFG.
  auto num_non_entry_blocks = cfg.num_blocks() - 1;

  size_t num_exit_calls;
  std::vector<IRInstruction*> invokes;
  if (options.instrumentation_strategy == "basic_block_hit_count") {
    // Step 5: Insert block counting instructions to each loop block then
    // insert it at the very end of the function for non-loop blocks for hit
    // counting
    always_assert(reg_vectors.size() == loop_shorts.size());
    int index = 0;
    size_t index_temp = 0;

    for (index_temp = 0; index_temp < loop_shorts.size(); index_temp++) {
      loop_shorts[index_temp] = 0;
    }

    for (const auto& i : blocks) {
      if (i.is_instrumentable()) {
        if (i.loop != nullptr) {
          int vec_index = index / 16;
          int bit = index % 16;

          loop_shorts[vec_index] |= (1 << bit);
        }
        index += 1;
      }
    }

    // onNonLoopBlockHit map
    invokes = insert_hit_count_insts(cfg, onBlockHit, hit_offset, blocks,
                                     num_instrument_loop_blocks);
    if (options.inline_onBlockHit) {
      TRACE(INSTRUMENT, 4, "Inline onBlockHits\n");
      std::unordered_set<IRInstruction*> insns(invokes.begin(), invokes.end());
      inliner.inline_callees(method, insns);
    }

    std::tie(num_exit_calls, invokes) = insert_onMethodExit_calls(
        method, cfg, reg_vectors, hit_offset, reg_hit_offset,
        onNonLoopBlockHit_map, onNonLoopBlockHit_map, max_vector_arity_hit,
        loop_shorts, options);
    if (options.inline_onNonLoopBlockHit) {
      TRACE(INSTRUMENT, 4, "Inline onNonLoopBlockHit\n");
      std::unordered_set<IRInstruction*> insns(invokes.begin(), invokes.end());
      inliner.inline_callees(method, insns);
    }
  } else {
    // Step 5: Insert onMethodExit in exit block(s) if basic block tracing is
    // enabled.
    //
    // TODO: What about no exit blocks possibly due to infinite loops? Such
    // case is extremely rare in our apps. In this case, let us do method
    // tracing by instrumenting prologues.
    std::tie(num_exit_calls, invokes) = insert_onMethodExit_calls(
        method, cfg, reg_vectors, method_offset, reg_method_offset,
        onMethodExit_map, onMethodExitUnchecked_map, max_vector_arity,
        loop_shorts, options);
  }
  cfg.recompute_registers_size();

  auto count = [&blocks](BlockType type) -> size_t {
    return std::count_if(blocks.begin(), blocks.end(), [type](const auto& i) {
      return (i.type & type) == type;
    });
  };

  // When there are too many blocks, collect all source blocks into the entry
  // block to track them conservatively.
  info.entry_source_blocks = too_many_blocks ? [&]() {
    std::vector<SourceBlock*> all;
    for (auto* b : cfg.blocks()) {
      auto tmp = source_blocks::gather_source_blocks(b);
      all.insert(all.end(), tmp.begin(), tmp.end());
    }
    return all;
  }() : source_blocks::gather_source_blocks(cfg.entry_block());
  info.too_many_blocks = too_many_blocks;
  info.num_too_many_blocks = too_many_blocks ? 1 : 0;
  info.offset = method_offset;
  info.hit_offset = hit_offset;
  info.num_non_entry_blocks = num_non_entry_blocks;
  info.num_vectors = num_vectors;
  info.num_hit_blocks = num_instrument_hit_blocks;
  info.num_loop_blocks = num_instrument_loop_blocks;
  info.num_exit_calls = num_exit_calls;
  info.num_empty_blocks = count(BlockType::Empty);
  info.num_useless_blocks = count(BlockType::Useless);
  info.num_no_source_blocks = count(BlockType::NoSourceBlock);
  info.num_blocks_too_large = too_many_blocks ? info.num_non_entry_blocks : 0;
  info.num_catches =
      count(BlockType::Catch) - count(BlockType::Catch | BlockType::Useless);
  info.num_instrumented_catches =
      count(BlockType::Catch | BlockType::Instrumentable);
  info.num_instrumented_blocks = num_to_instrument;
  always_assert(count(BlockType::Instrumentable) == num_to_instrument);

  redex_assert(std::none_of(blocks.begin(), blocks.end(), [](const auto& b) {
    return std::find(b.merge_in.begin(), b.merge_in.end(), b.block) !=
           b.merge_in.end();
  }));
  info.num_merged = std::accumulate(
      blocks.begin(), blocks.end(), 0,
      [](auto lhs, const auto& rhs) { return lhs + rhs.merge_in.size(); });
  info.num_merged_not_instrumented = std::accumulate(
      blocks.begin(), blocks.end(), 0, [](auto lhs, const auto& rhs) {
        return lhs + ((rhs.type & BlockType::Instrumentable) !=
                              BlockType::Instrumentable
                          ? rhs.merge_in.size()
                          : 0);
      });

  const size_t num_rejected_blocks =
      info.num_empty_blocks + info.num_useless_blocks +
      info.num_no_source_blocks + info.num_blocks_too_large +
      (info.num_catches - info.num_instrumented_catches);
  always_assert(info.num_non_entry_blocks ==
                info.num_instrumented_blocks + num_rejected_blocks);
  always_assert(too_many_blocks ||
                info.rejected_blocks.size() == num_rejected_blocks);

  TRACE(INSTRUMENT, DEBUG_CFG ? 0 : 10, "AFTER: %s, %s\n%s",
        show_deobfuscated(method).c_str(), SHOW(method), SHOW(cfg));

  // Check the post condition:
  //   num_instrumented_blocks == num_non_entry_blocks - num_rejected_blocks
  if (get_instrumented_type(info) != InstrumentedType::MethodOnly &&
      num_to_instrument !=
          info.num_non_entry_blocks - info.rejected_blocks.size()) {
    TRACE(INSTRUMENT, 7, "Post condition violation! in %s", SHOW(method));
    TRACE(INSTRUMENT, 7, "- Instrumented type: %d",
          (int)get_instrumented_type(info));
    TRACE(INSTRUMENT, 7, "  %zu != %zu - %zu", num_to_instrument,
          info.num_non_entry_blocks, info.rejected_blocks.size());
    TRACE(INSTRUMENT, 7, "  original non-entry blocks: %zu",
          origin_num_non_entry_blocks);
    TRACE(INSTRUMENT, 7, "  after prologue instrumentation: %zu",
          after_prologue_num_non_entry_blocks);
    TRACE(INSTRUMENT, 7, "===== BEFORE CFG");
    TRACE(INSTRUMENT, 7, "%s", SHOW(before_cfg));
    TRACE(INSTRUMENT, 7, "===== AFTER CFG");
    TRACE(INSTRUMENT, 7, "%s", SHOW(cfg));
  }

  code.clear_cfg();
  return info;
}

std::unordered_set<std::string> get_cold_start_classes(ConfigFiles& cfg) {
  auto interdex_list = cfg.get_coldstart_classes();
  std::unordered_set<std::string> cold_start_classes;
  std::string dex_end_marker0("LDexEndMarker0;");
  for (auto class_string : interdex_list) {
    if (class_string == dex_end_marker0) {
      break;
    }
    class_string.back() = '/';
    cold_start_classes.insert(class_string);
  }
  return cold_start_classes;
}

void print_stats(ScopedMetrics& sm,
                 const std::vector<MethodInfo>& instrumented_methods,
                 const size_t max_num_blocks) {
  MethodInfo total{};
  for (const auto& i : instrumented_methods) {
    total += i;
  }

  const size_t total_instrumented = instrumented_methods.size();
  const size_t only_method_instrumented = total.num_too_many_blocks;
  const size_t total_block_instrumented =
      total_instrumented - only_method_instrumented;

  auto print = [](size_t num, size_t total, size_t& accumulate) {
    std::stringstream ss;
    accumulate += num;
    ss << std::fixed << std::setprecision(3) << std::setw(6) << num << " ("
       << std::setw(6) << (num * 100. / total) << "%, " << std::setw(6)
       << (accumulate * 100. / total) << "%)";
    return ss.str();
  };

  auto divide = [](size_t a, size_t b) {
    if (b == 0) {
      return std::string("N/A");
    }
    std::stringstream ss;
    ss << std::fixed << std::setprecision(4) << double(a) / double(b);
    return ss.str();
  };

  // ----- Print summary
  {
    auto summary_scope = sm.scope("summary");
    TRACE(INSTRUMENT, 4, "Maximum blocks for block instrumentation: %zu",
          max_num_blocks);
    sm.set_metric("max_num_blocks", max_num_blocks);
    TRACE(INSTRUMENT, 4, "Total instrumented methods: %zu", total_instrumented);
    sm.set_metric("total_instrumented", total_instrumented);
    TRACE(INSTRUMENT, 4, "- Block + method instrumented: %zu",
          total_block_instrumented);
    sm.set_metric("block_and_method_instrumented", total_block_instrumented);
    TRACE(INSTRUMENT, 4, "- Only method instrumented: %zu",
          only_method_instrumented);
    sm.set_metric("method_instrumented_only", only_method_instrumented);
  }

  auto scope_total_avg = [&](const std::string& key, size_t num, size_t denom) {
    auto scope = sm.scope(key);
    sm.set_metric("total", num);
    if (denom != 0) {
      sm.set_metric("average100", 100 * num / denom);
    }
    return scope;
  };

  // ----- Bit-vector stats
  TRACE(INSTRUMENT, 4, "Bit-vector stats for block instrumented methods:");
  {
    size_t acc = 0;
    size_t total_bit_vectors = 0;
    std::map<int /*num_vectors*/, size_t /*num_methods*/> dist;
    for (const auto& i : instrumented_methods) {
      if (i.too_many_blocks) {
        ++dist[-1];
      } else {
        ++dist[i.num_vectors];
        total_bit_vectors += i.num_vectors;
      }
    }
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %3d vectors: %s", p.first,
            SHOW(print(p.second, total_instrumented, acc)));
    }
    TRACE(INSTRUMENT, 4, "Total/average bit vectors: %zu, %s",
          total_bit_vectors,
          SHOW(divide(total_bit_vectors, total_block_instrumented)));
    scope_total_avg("bit_vectors", total_bit_vectors, total_block_instrumented);
  }

  // ----- Instrumented block stats
  TRACE(INSTRUMENT, 4, "Instrumented / actual non-entry block stats:");

  {
    std::map<int, std::pair<size_t /*instrumented*/, size_t /*block*/>> dist;
    for (const auto& i : instrumented_methods) {
      if (i.too_many_blocks) {
        ++dist[-1].first;
      } else {
        ++dist[i.num_instrumented_blocks].first;
      }
      ++dist[i.num_non_entry_blocks].second;
    }
    std::array<size_t, 2> accs = {0, 0};
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %5d blocks: %s | %s", p.first,
            SHOW(print(p.second.first, total_instrumented, accs[0])),
            SHOW(print(p.second.second, total_instrumented, accs[1])));
    }
    TRACE(
        INSTRUMENT, 4, "Total/average instrumented blocks: %zu, %s",
        total.num_instrumented_blocks,
        SHOW(divide(total.num_instrumented_blocks, total_block_instrumented)));
    scope_total_avg("instrumented_blocks", total.num_instrumented_blocks,
                    total_block_instrumented);
    TRACE(INSTRUMENT, 4, "Total/average non-entry blocks: %zu, %s",
          total.num_non_entry_blocks,
          SHOW(divide(total.num_non_entry_blocks, total_instrumented)));
    scope_total_avg("non_entry_blocks", total.num_non_entry_blocks,
                    total_block_instrumented);
  }

  const size_t total_catches = std::accumulate(
      instrumented_methods.begin(), instrumented_methods.end(), size_t(0),
      [](int a, auto&& i) { return a + i.num_catches; });
  const size_t total_instrumented_catches = std::accumulate(
      instrumented_methods.begin(), instrumented_methods.end(), size_t(0),
      [](int a, auto&& i) { return a + i.num_instrumented_catches; });

  // ----- Instrumented loop block stats
  TRACE(INSTRUMENT, 4, "Instrumented Loop Block stats:");

  {
    size_t acc = 0;
    size_t total_num_loop_blocks = 0;
    std::map<int /*num_vectors*/, size_t /*num_methods*/> dist;
    for (const auto& i : instrumented_methods) {
      if (i.too_many_blocks) {
        ++dist[-1];
      } else {
        ++dist[i.num_loop_blocks];
        total_num_loop_blocks += i.num_loop_blocks;
      }
    }
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %3d loop blocks: %s", p.first,
            SHOW(print(p.second, total_instrumented, acc)));
    }
    TRACE(INSTRUMENT, 4, "Total/average instrumented loop blocks: %zu, %s",
          total_num_loop_blocks,
          SHOW(divide(total_num_loop_blocks, total_block_instrumented)));
    scope_total_avg("loop_blocks", total_num_loop_blocks,
                    total_block_instrumented);
  }

  // ----- Instrumented/skipped block stats
  auto print_ratio = [&total](size_t num) {
    std::stringstream ss;
    ss << num << std::fixed << std::setprecision(2) << " ("
       << (num * 100. / total.num_non_entry_blocks) << "%)";
    return ss.str();
  };
  auto metric_ratio = [&sm, &total](const std::string& sub_key, size_t num) {
    if (total.num_non_entry_blocks == 0) {
      return;
    }
    sm.set_metric(sub_key, num);
    sm.set_metric(sub_key + ".ratio100.00",
                  10000 * num / total.num_non_entry_blocks);
  };

  {
    auto non_entry_scope = sm.scope("non_entry_blocks_stats");
    TRACE(INSTRUMENT, 4, "Total non-entry blocks: %zu",
          total.num_non_entry_blocks);
    sm.set_metric("total", total.num_non_entry_blocks);
    TRACE(INSTRUMENT, 4, "- Instrumented blocks: %s",
          SHOW(print_ratio(total.num_instrumented_blocks)));
    metric_ratio("total_instrumented_blocks", total.num_instrumented_blocks);
    TRACE(INSTRUMENT, 4, "- Merged blocks: %s",
          print_ratio(total.num_merged).c_str());
    sm.set_metric("merged", total.num_merged);
    TRACE(INSTRUMENT, 4, "- Merged blocks (into non-instrumentable): %s",
          print_ratio(total.num_merged_not_instrumented).c_str());
    sm.set_metric("merged_not_instrumentable",
                  total.num_merged_not_instrumented);
    TRACE(INSTRUMENT, 4, "- Skipped catch blocks: %s",
          SHOW(print_ratio(total_catches - total_instrumented_catches)));
    {
      auto skipped_scope = sm.scope("skipped");
      metric_ratio("catch_blocks", total_catches - total_instrumented_catches);
      auto no_sb = std::accumulate(
          instrumented_methods.begin(), instrumented_methods.end(), size_t(0),
          [](size_t a, auto&& i) { return a + i.num_no_source_blocks; });
      TRACE(INSTRUMENT, 4, "- Skipped due to no source block: %s",
            SHOW(print_ratio(no_sb)));
      metric_ratio("no_source_blocks", no_sb);
      auto too_large_methods = std::accumulate(
          instrumented_methods.begin(), instrumented_methods.end(), size_t(0),
          [](size_t a, auto&& i) { return a + i.num_blocks_too_large; });
      TRACE(INSTRUMENT, 4, "- Skipped due to too large methods: %s",
            SHOW(print_ratio(too_large_methods)));
      metric_ratio("too_large_methods", too_large_methods);
      auto empty_blocks = std::accumulate(
          instrumented_methods.begin(), instrumented_methods.end(), size_t(0),
          [](size_t a, auto&& i) { return a + i.num_empty_blocks; });
      TRACE(INSTRUMENT, 4, "- Skipped empty blocks: %s",
            SHOW(print_ratio(empty_blocks)));
      metric_ratio("empty_blocks", empty_blocks);
      auto useless_blocks = std::accumulate(
          instrumented_methods.begin(), instrumented_methods.end(), size_t(0),
          [](size_t a, auto&& i) { return a + i.num_useless_blocks; });
      TRACE(INSTRUMENT, 4, "- Skipped useless blocks: %s",
            SHOW(print_ratio(useless_blocks)));
      metric_ratio("useless_blocks", useless_blocks);
    }
  }

  // ----- Instrumented exit block stats
  TRACE(INSTRUMENT, 4, "Instrumented exit block stats:");
  {
    size_t acc = 0;
    size_t total_exits = 0;
    size_t no_exit = 0;
    std::map<int /*num_vectors*/, size_t /*num_methods*/> dist;
    TRACE(INSTRUMENT, 4, "No onMethodExit but 1+ non-entry blocks:");
    int k = 0;
    for (const auto& i : instrumented_methods) {
      if (!i.too_many_blocks && i.num_exit_calls == 0 &&
          i.num_non_entry_blocks != 0) {
        TRACE(INSTRUMENT, 4, "- %d: %zu, %s", ++k, i.num_non_entry_blocks,
              show_deobfuscated(i.method).c_str());
        ++no_exit;
      }
      ++dist[i.num_exit_calls];
      total_exits += i.num_exit_calls;
    }
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %4d exits: %s", p.first,
            SHOW(print(p.second, total_instrumented, acc)));
    }
    TRACE(INSTRUMENT, 4, "Total/average instrumented exits: %zu, %s",
          total_exits, SHOW(divide(total_exits, total_instrumented)));
    auto exit_scope =
        scope_total_avg("instrumented_exits", total_exits, total_instrumented);
    sm.set_metric("methods_without_exit_calls", no_exit);
  }

  // ----- Catch block stats
  TRACE(INSTRUMENT, 4, "Catch block stats:");
  {
    size_t acc = 0;
    std::map<int, size_t> dist;
    for (const auto& i : instrumented_methods) {
      ++dist[i.num_catches];
    }
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %4d catches: %s", p.first,
            SHOW(print(p.second, total_instrumented, acc)));
    }
    TRACE(INSTRUMENT, 4, "Total/average catch blocks: %zu, %s",
          total.num_catches,
          SHOW(divide(total.num_catches, total_instrumented)));
    scope_total_avg("catch_blocks", total.num_catches, total_instrumented);
  }

  auto print_two_dists = [&divide, &print, &instrumented_methods,
                          total_instrumented, total_block_instrumented](
                             const char* name1, const char* name2,
                             auto&& accessor1, auto&& accessor2) {
    std::map<int, std::pair<size_t, size_t>> dist;
    size_t total1 = 0;
    size_t total2 = 0;
    for (const auto& i : instrumented_methods) {
      if (i.too_many_blocks) {
        ++dist[-1].first;
        ++dist[-1].second;
      } else {
        ++dist[accessor1(i)].first;
        ++dist[accessor2(i)].second;
        total1 += accessor1(i);
        total2 += accessor2(i);
      }
    }
    std::array<size_t, 2> accs = {0, 0};
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %5d blocks: %s | %s", p.first,
            SHOW(print(p.second.first, total_instrumented, accs[0])),
            SHOW(print(p.second.second, total_instrumented, accs[1])));
    }
    TRACE(INSTRUMENT, 4, "Total/average %s blocks: %zu, %s", name1, total1,
          SHOW(divide(total1, total_block_instrumented)));
    TRACE(INSTRUMENT, 4, "Total/average %s blocks: %zu, %s", name2, total2,
          SHOW(divide(total2, total_block_instrumented)));
  };

  TRACE(INSTRUMENT, 4, "Empty / useless block stats:");
  print_two_dists(
      "empty", "useless", [](auto&& v) { return v.num_empty_blocks; },
      [](auto&& v) { return v.num_useless_blocks; });
}

} // namespace

//------------------------------------------------------------------------------
// A simple basic block instrumentation algorithm using bit vectors:
//
// Original CFG:
//   +--------+       +--------+       +--------+
//   | block0 | ----> | block1 | ----> | block2 |
//   |        |       |        |       | Return |
//   +--------+       +--------+       +--------+
//
// This CFG is instrumented as following:
//  - Insert instructions to initialize bit vector(s) at the entry block.
//  - Set <bb_id>-th bit in the vector using or-lit/16. The bit vector is a
//    short type. There is no such or-lit/32 instruction.
//  - Before RETURN, insert INVOKE DynamicAnalysis.onMethodExit(method_id,
//    bit_vectors), where the recorded bit vectors are reported.
//
//   +------------------+     +------------------+     +-----------------------+
//   | * CONST v0, 0    | --> | * OR_LIT16 v0, 2 | --> | * OR_LIT16 v0, 4      |
//   | * OR_LIT16 v0, 1 |     |   block1         |     |   block2              |
//   |   block0         |     |                  |     | * CONST v2, method_id |
//   +------------------+     +------------------+     | * INVOKE v2,v0, ...   |
//                                                     |   Return              |
//                                                     +-----------------------+
//
// This instrumentation includes the method tracing by inserting onMethodBegin.
// We currently don't instrument methods with large number of basic blocks. In
// this case, they are only instrumented for method tracing.
//------------------------------------------------------------------------------
void BlockInstrumentHelper::do_basic_block_tracing(
    DexClass* analysis_cls,
    DexStoresVector& stores,
    ConfigFiles& cfg,
    PassManager& pm,
    const InstrumentPass::Options& options) {
  // Do not allow semantics-changing reorderings anymore. They may move the
  // instrumentation instructions relative to their anchors. An example is
  // new-instance normalization.
  g_redex->set_ordering_changes_allowed(false);

  // I'm too lazy to support sharding in block instrumentation. Future work.
  const size_t NUM_SHARDS = options.num_shards;
  if (NUM_SHARDS != 1 || options.num_stats_per_method != 0) {
    always_assert_log(
        false,
        "[InstrumentPass] error: basic block profiling currently only "
        "supports num_shard = 1 and num_stats_per_method = 0");
  }
  if (options.analysis_method_names.size() < 3) {
    always_assert_log(
        false,
        "[InstrumentPass] error: basic block profiling must have "
        "two analysis methods: [onMethodBegin, onMethodExit, onBlockHit]");
  }

  const size_t max_num_blocks = options.max_num_blocks;

  // Even so, we need to update sharded arrays with 1 for the Java-side code.
  const auto& array_fields = InstrumentPass::patch_sharded_arrays(
      analysis_cls, NUM_SHARDS,
      // However, because we have only one shard and don't clone onMethodExits,
      // we keep the original name. It actually fools patch_sharded_arrays.
      {{1, InstrumentPass::STATS_FIELD_NAME}});
  always_assert(array_fields.size() == NUM_SHARDS);

  DexMethod* onMethodBegin =
      load_onMethodBegin(*analysis_cls, options.analysis_method_names[0]);
  TRACE(INSTRUMENT, 4, "Loaded onMethodBegin: %s", SHOW(onMethodBegin));

  const auto& onMethodExit_map = build_onMethodExit_map(
      *analysis_cls, options.analysis_method_names[1], type::_boolean());
  const size_t max_vector_arity = onMethodExit_map.rbegin()->first;
  TRACE(INSTRUMENT, 4, "Max arity for onMethodExit: %zu", max_vector_arity);

  const auto& onMethodExitUnchecked_map = build_onMethodExit_map(
      *analysis_cls, options.analysis_method_names[2], type::_void());
  // For simplicity we expect the same checked and unchecked max method arities.
  always_assert_log(max_vector_arity ==
                        onMethodExitUnchecked_map.rbegin()->first,
                    "%zu != %zu", max_vector_arity,
                    onMethodExitUnchecked_map.rbegin()->first);

  DexMethod* onBlockHit =
      load_onMethodBegin(*analysis_cls, options.analysis_method_names[3]);
  TRACE(INSTRUMENT, 4, "Loaded onBlockHit: %s", SHOW(onBlockHit));

  const auto& onNonLoopBlockHit_map = build_onMethodExit_map(
      *analysis_cls, options.analysis_method_names[4], type::_void());
  const size_t max_vector_arity_other = onMethodExit_map.rbegin()->first;
  TRACE(INSTRUMENT, 4, "Max arity for onNonLoopBlockHit: %zu",
        max_vector_arity_other);

  auto cold_start_classes = get_cold_start_classes(cfg);
  TRACE(INSTRUMENT, 7, "Cold start classes: %zu", cold_start_classes.size());

  // Create Buildable CFG so we can inline functions correctly.
  if (options.inline_onBlockHit) {
    IRCode* blockHit_code = onBlockHit->get_code();
    blockHit_code->build_cfg(true);
  }

  for (auto& en : onNonLoopBlockHit_map) {
    IRCode* nonLoopBlockHit_code = en.second->get_code();
    nonLoopBlockHit_code->build_cfg(true);
  }

  DexMethod* binaryIncrementer;
  for (const auto& m : analysis_cls->get_dmethods()) {
    const auto name = m->get_name()->str();
    if ("binaryIncrementer" != name) {
      continue;
    }
    const auto* args = m->get_proto()->get_args();
    if (args->size() != 2) {
      always_assert_log(
          false,
          "[InstrumentPass] error: Proto type of binaryIncrementer must be "
          "binaryIncrementer(int, short), but it was %s",
          show(m->get_proto()).c_str());
    }
    binaryIncrementer = m;
    break;
  }
  IRCode* binaryIncrementer_code = binaryIncrementer->get_code();
  binaryIncrementer_code->build_cfg(true);

  // This method_offset is used in sMethodStats[] to locate a method profile.
  // We have a small header in the beginning of sMethodStats.
  size_t method_offset = 8;
  size_t hit_offset = 8;
  std::vector<MethodInfo> instrumented_methods;

  int all_methods = 0;
  int eligibles = 0;
  int specials = 0;
  int picked_by_cs = 0;
  int picked_by_allowlist = 0;
  int blocklisted = 0;
  int rejected = 0;
  int block_instrumented = 0;
  int non_root_store_methods = 0;

  Scope scope;
  if (options.instrument_only_root_store) {
    DexStoresVector root;
    for (const auto& store : stores) {
      if (store.is_root_store()) {
        root.push_back(store);
      } else {
        // We want to collect number of methods that are being excluded.
        for (const auto& cls : build_class_scope({store})) {
          non_root_store_methods +=
              cls->get_dmethods().size() + cls->get_vmethods().size();
        }
      }
    }
    all_methods += non_root_store_methods;
    scope = build_class_scope(root);
  } else {
    scope = build_class_scope(stores);
  }

  // Inlining Code
  auto method_override_graph = method_override_graph::build_graph(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, cfg.create_init_class_insns(), method_override_graph.get());

  ConcurrentMethodResolver concurrent_method_resolver;

  std::unordered_set<DexMethod*> no_default_inlinables;
  auto inliner_config = cfg.get_inliner_config();
  int min_sdk = pm.get_redex_options().min_sdk;
  MultiMethodInliner inliner(
      scope, init_classes_with_side_effects, stores, no_default_inlinables,
      std::ref(concurrent_method_resolver), inliner_config, min_sdk,
      MultiMethodInlinerMode::None);

  for (auto& en : onNonLoopBlockHit_map) {
    std::unordered_set<DexMethod*> insns;
    insns.insert(binaryIncrementer);
    inliner.inline_callees(en.second, insns);
  }

  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    TraceContext trace_context(method);

    all_methods++;
    if (method == analysis_cls->get_clinit() || method == onMethodBegin ||
        method == onBlockHit || method == binaryIncrementer) {
      specials++;
      return;
    }

    if (std::any_of(onMethodExit_map.begin(), onMethodExit_map.end(),
                    [&](const auto& e) { return e.second == method; })) {
      specials++;
      return;
    }
    if (std::any_of(onMethodExitUnchecked_map.begin(),
                    onMethodExitUnchecked_map.end(),
                    [&](const auto& e) { return e.second == method; })) {
      specials++;
      return;
    }

    if (std::any_of(onNonLoopBlockHit_map.begin(), onNonLoopBlockHit_map.end(),
                    [&](const auto& e) { return e.second == method; })) {
      specials++;
      return;
    }

    eligibles++;
    if (!options.allowlist.empty() || options.only_cold_start_class) {
      if (InstrumentPass::is_included(method, options.allowlist)) {
        picked_by_allowlist++;
      } else if (InstrumentPass::is_included(method, cold_start_classes)) {
        picked_by_cs++;
      } else {
        // We are using allow or cs list. If not there, reject.
        rejected++;
        TRACE(INSTRUMENT, 9, "Not in allow/cold_start: %s, %s",
              show_deobfuscated(method).c_str(), SHOW(method));
        return;
      }
    }

    // Here, `method` is either allow listed or no allowlist. Blocklist has
    // priority over allowlist or cold start list. So, check additionally.
    if (InstrumentPass::is_included(method, options.blocklist)) {
      blocklisted++;
      TRACE(INSTRUMENT, 9, "Blocklisted: %s, %s",
            show_deobfuscated(method).c_str(), SHOW(method));
      return;
    }

    instrumented_methods.emplace_back(instrument_basic_blocks(
        code, method, onMethodBegin, onMethodExit_map,
        onMethodExitUnchecked_map, onBlockHit, onNonLoopBlockHit_map,
        max_vector_arity, max_vector_arity_other, method_offset, hit_offset,
        max_num_blocks, inliner, options));

    const auto& method_info = instrumented_methods.back();
    if (method_info.too_many_blocks) {
      TRACE(INSTRUMENT, 7, "Too many blocks: %s",
            SHOW(show_deobfuscated(method)));
    } else {
      block_instrumented++;
    }

    // Update method offset for next method. 2 shorts are for method stats.
    method_offset += 2 + method_info.num_vectors;
    hit_offset += method_info.num_hit_blocks;
  });

  // Destroy the CFG because we are done Instrumenting
  if (options.inline_onBlockHit) {
    IRCode* blockHit_code = onBlockHit->get_code();
    blockHit_code->clear_cfg();
  }

  for (auto& en : onNonLoopBlockHit_map) {
    IRCode* nonLoopBlockHit_code = en.second->get_code();
    nonLoopBlockHit_code->clear_cfg();
  }

  binaryIncrementer_code->clear_cfg();

  // Patch static fields.
  const auto field_name = array_fields.at(1)->get_name()->str();
  InstrumentPass::patch_array_size(analysis_cls, field_name, method_offset);

  auto* field = analysis_cls->find_field_from_simple_deobfuscated_name(
      "sNumStaticallyInstrumented");
  always_assert(field != nullptr);
  InstrumentPass::patch_static_field(analysis_cls, field->get_name()->str(),
                                     instrumented_methods.size());

  field =
      analysis_cls->find_field_from_simple_deobfuscated_name("sProfileType");
  always_assert(field != nullptr);
  InstrumentPass::patch_static_field(
      analysis_cls, field->get_name()->str(),
      static_cast<int>(ProfileTypeFlags::BasicBlockTracing));

  if (options.instrumentation_strategy == "basic_block_hit_count") {
    field = analysis_cls->find_field_from_simple_deobfuscated_name("sHitStats");
    InstrumentPass::patch_array_size(analysis_cls, field->get_name()->str(),
                                     hit_offset);

    field = analysis_cls->find_field_from_simple_deobfuscated_name(
        "sNumStaticallyHitsInstrumented");
    always_assert(field != nullptr);
    InstrumentPass::patch_static_field(analysis_cls, field->get_name()->str(),
                                       hit_offset - 8);

    field =
        analysis_cls->find_field_from_simple_deobfuscated_name("sProfileType");
    always_assert(field != nullptr);
    InstrumentPass::patch_static_field(
        analysis_cls, field->get_name()->str(),
        static_cast<int>(ProfileTypeFlags::BasicBlockHitCount));
  }

  write_metadata(cfg, options.metadata_file_name, instrumented_methods,
                 options.instrumentation_strategy);

  ScopedMetrics sm(pm);
  auto block_instr_scope = sm.scope("block_instr");

  print_stats(sm, instrumented_methods, max_num_blocks);

  {
    auto methods_scope = sm.scope("methods");
    TRACE(INSTRUMENT, 4, "Instrumentation selection stats:");
    TRACE(INSTRUMENT, 4, "- All methods: %d", all_methods);
    sm.set_metric("all", all_methods);
    TRACE(INSTRUMENT, 4, "- Eligible methods: %d", eligibles);
    sm.set_metric("eligible", eligibles);
    TRACE(INSTRUMENT, 4, "  Uninstrumentable methods: %d", specials);
    sm.set_metric("special", specials);
    TRACE(INSTRUMENT, 4, "  Non-root methods: %d", non_root_store_methods);
    sm.set_metric("non_root", non_root_store_methods);
  }
  {
    auto sel_scope = sm.scope("selected");
    TRACE(INSTRUMENT, 4, "- Explicitly selected:");
    TRACE(INSTRUMENT, 4, "  Allow listed: %d", picked_by_allowlist);
    sm.set_metric("allow_list", picked_by_allowlist);
    TRACE(INSTRUMENT, 4, "  Cold start: %d", picked_by_cs);
    sm.set_metric("cold_start", picked_by_cs);
  }
  {
    auto rej_scope = sm.scope("rejected");
    TRACE(INSTRUMENT, 4, "- Explicitly rejected:");
    TRACE(INSTRUMENT, 4, "  Not in allow or cold start set: %d", rejected);
    sm.set_metric("not_allow_or_cold_start", rejected);
    TRACE(INSTRUMENT, 4, "  Block listed: %d", blocklisted);
    sm.set_metric("block_list", blocklisted);
  }
}
