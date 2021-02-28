/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockInstrument.h"

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "MethodReference.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <boost/algorithm/string/join.hpp>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace instrument;

namespace {

constexpr bool DEBUG_CFG = false;
constexpr size_t BIT_VECTOR_SIZE = 16;
constexpr int PROFILING_DATA_VERSION = 3;

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

using BitId = size_t;

struct BlockInfo {
  cfg::Block* block;
  BlockType type;
  IRList::iterator it;
  BitId bit_id;

  BlockInfo(cfg::Block* b, BlockType t, const IRList::iterator& i)
      : block(b), type(t), it(i), bit_id(std::numeric_limits<BitId>::max()) {}

  bool is_instrumentable() const {
    return (type & BlockType::Instrumentable) == BlockType::Instrumentable;
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
  size_t num_non_entry_blocks = 0;
  size_t num_vectors = 0;
  size_t num_exit_calls = 0;
  uint64_t signature = 0;

  size_t num_empty_blocks = 0;
  size_t num_useless_blocks = 0;
  size_t num_catches = 0;
  size_t num_instrumented_catches = 0;
  size_t num_instrumented_blocks = 0;

  std::vector<cfg::BlockId> bit_id_2_block_id;
  std::vector<std::vector<const SourceBlock*>> bit_id_2_source_blocks;
  std::map<cfg::BlockId, BlockType> rejected_blocks;
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

bool compare_dexmethods_by_deobname(const DexMethodRef* a,
                                    const DexMethodRef* b) {
  const auto& name_a = show_deobfuscated(a);
  const auto& name_b = show_deobfuscated(b);
  always_assert_log(a == b || name_a != name_b,
                    "Identical deobfuscated names were found: %s == %s",
                    name_a.c_str(), name_b.c_str());
  return name_a < name_b;
}

using MethodDictionary = std::unordered_map<const DexMethodRef*, size_t>;

MethodDictionary create_method_dictionary(
    const std::string& file_name, const std::vector<MethodInfo>& all_info) {
  std::unordered_set<const DexMethodRef*> methods_set;
  for (const auto& info : all_info) {
    methods_set.insert(info.method);
    for (const auto& sb_vec : info.bit_id_2_source_blocks) {
      for (const auto* sb : sb_vec) {
        methods_set.insert(sb->src);
      }
    }
  }
  std::vector<const DexMethodRef*> methods(methods_set.begin(),
                                           methods_set.end());
  std::sort(methods.begin(), methods.end(), compare_dexmethods_by_deobname);
  size_t idx{0};

  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  ofs << "type,version\nredex-source-block-method-dictionary,1\n";
  ofs << "index,deob_name\n";
  MethodDictionary method_dictionary;
  for (const auto* m : methods) {
    method_dictionary.emplace(m, idx);
    ofs << idx << "," << show_deobfuscated(m) << "\n";
    ++idx;
  }

  return method_dictionary;
}

void write_metadata(const ConfigFiles& cfg,
                    const std::string& metadata_base_file_name,
                    const std::vector<MethodInfo>& all_info) {
  const auto method_dict = create_method_dictionary(
      cfg.metafile("redex-source-block-method-dictionary.csv"), all_info);

  // Write a short metadata of this metadata file in the first two lines.
  auto file_name = cfg.metafile(metadata_base_file_name);
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  ofs << "profile_type,version,num_methods" << std::endl;
  ofs << "basic-block-tracing," << PROFILING_DATA_VERSION << ","
      << all_info.size() << std::endl;

  // The real CSV-style metadata follows.
  const std::array<std::string, 9> headers = {
      "offset",    "name",      "instrument",        "non_entry_blocks",
      "vectors",   "signature", "bit_id_2_block_id", "rejected_blocks",
      "src_blocks"};
  ofs << boost::algorithm::join(headers, ",") << "\n";

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

  auto source_blocks = [&method_dict](const auto& bit_id_2_source_blocks) {
    std::stringstream ss;
    bool first1 = true;
    for (const auto& v : bit_id_2_source_blocks) {
      if (first1) {
        first1 = false;
      } else {
        ss << ";";
      }

      bool first2 = true;
      for (auto* sb : v) {
        if (first2) {
          first2 = false;
        } else {
          ss << "|";
        }
        ss << method_dict.at(sb->src) << "#" << sb->id;
      }
    }
    return ss.str();
  };

  for (const auto& info : all_info) {
    const std::array<std::string, 9> fields = {
        std::to_string(info.offset),
        std::to_string(method_dict.at(info.method)),
        std::to_string(static_cast<int>(get_instrumented_type(info))),
        std::to_string(info.num_non_entry_blocks),
        std::to_string(info.num_vectors),
        to_hex(info.signature),
        write_block_id_map(info.bit_id_2_block_id),
        rejected_blocks(info.rejected_blocks),
        source_blocks(info.bit_id_2_source_blocks),
    };
    ofs << boost::algorithm::join(fields, ",") << "\n";
  }

  TRACE(INSTRUMENT, 2, "Metadata file was written to: %s", SHOW(file_name));
}

uint64_t compute_cfg_signature(const std::vector<BlockInfo>& blocks) {
  // Blocks should be sorted in a deterministic way like a RPO.
  // Encode block shapes with opcodes lists per block.
  std::ostringstream serialized;
  for (const auto& info : blocks) {
    const cfg::Block* b = info.block;
    serialized << b->id();
    for (const auto& p : b->preds()) {
      serialized << p->src()->id();
    }
    for (const auto& s : b->succs()) {
      serialized << s->src()->id();
    }
    for (const auto& i : *b) {
      if (i.type == MFLOW_OPCODE) {
        // Don't write srcs and dests. Too much. Just opcode would be enough.
        serialized << static_cast<uint16_t>(i.insn->opcode());
      }
    }
  }
  return std::hash<std::string>{}(serialized.str());
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

OnMethodExitMap build_onMethodExit_map(const DexClass& cls,
                                       const std::string& onMethodExit_name) {
  OnMethodExitMap onMethodExit_map;
  for (const auto& m : cls.get_dmethods()) {
    const auto& name = m->get_name()->str();
    if (onMethodExit_name != name) {
      continue;
    }

    // The prototype of onMethodExit must be one of:
    // - onMethodExit(int offset), or
    // - onMethodExit(int offset, short vec1, ..., short vecN);
    const auto* args = m->get_proto()->get_args();
    if (args->size() == 0 ||
        *args->get_type_list().begin() != DexType::make_type("I") ||
        std::any_of(std::next(args->get_type_list().begin(), 1),
                    args->get_type_list().end(), [](const auto& type) {
                      return type != DexType::make_type("S");
                    })) {
      always_assert_log(
          false,
          "[InstrumentPass] error: Proto type of onMethodExit must be "
          "(int) or (int, short, ..., short), but it was %s",
          show(m->get_proto()).c_str());
    }

    // -1 is to exclude `int offset`.
    onMethodExit_map[args->size() - 1] = m;
  }

  if (onMethodExit_map.empty()) {
    std::stringstream ss;
    for (const auto& m : cls.get_dmethods()) {
      ss << " " << show(m) << std::endl;
    }
    always_assert_log(false,
                      "[InstrumentPass] error: cannot find %s in %s:\n%s",
                      onMethodExit_name.c_str(), SHOW(cls), ss.str().c_str());
  }

  return onMethodExit_map;
}

DexMethod* load_onMethodBegin(const DexClass& cls,
                              const std::string& method_name) {
  for (const auto& m : cls.get_dmethods()) {
    const auto& name = m->get_name()->str();
    if (method_name != name) {
      continue;
    }
    const auto* args = m->get_proto()->get_args();
    if (args->size() != 1 ||
        *args->get_type_list().begin() != DexType::make_type("I")) {
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
                           const size_t method_offset) {
  std::vector<reg_t> reg_vectors(num_vectors);
  std::vector<IRInstruction*> prologues(num_vectors + 2);

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

  IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_inst->set_method(onMethodBegin);
  invoke_inst->set_srcs_size(1);
  invoke_inst->set_src(0, reg_method_offset);
  prologues.at(num_vectors + 1) = invoke_inst;

  // Insert all prologue opcodes to the entry block (right after param loading).
  cfg.entry_block()->insert_before(
      cfg.entry_block()->to_cfg_instruction_iterator(
          cfg.entry_block()->get_first_non_param_loading_insn()),
      prologues);

  return std::make_tuple(reg_vectors, reg_method_offset);
}

size_t insert_onMethodExit_calls(
    cfg::ControlFlowGraph& cfg,
    const std::vector<reg_t>& reg_vectors, // May be empty
    const size_t method_offset,
    const reg_t reg_method_offset,
    const std::map<size_t, DexMethod*>& onMethodExit_map,
    const size_t max_vector_arity) {
  // If reg_vectors is emptry (methods with a single entry block), no need to
  // instrument onMethodExit.
  if (reg_vectors.empty()) {
    return 0;
  }

  // When a method exits, we call onMethodExit to pass all vectors to record.
  // onMethodExit is overloaded to some degrees (e.g., up to 5 vectors). If
  // number of vectors > 5, generate one or more onMethodExit calls.
  const size_t num_vectors = reg_vectors.size();
  const size_t num_invokes =
      std::max(1., std::ceil(double(num_vectors) / double(max_vector_arity)));

  auto create_invoke_insts = [&]() -> auto {
    // This code works in case of num_invokes == 1.
    std::vector<IRInstruction*> invoke_insts(num_invokes * 2 - 1);
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
      invoke_insts.at(i * 2) = inst;

      if (i != num_invokes - 1) {
        inst = new IRInstruction(OPCODE_CONST);
        // Move forward the offset.
        offset += max_vector_arity;
        inst->set_literal(offset);
        inst->set_dest(reg_method_offset);
        invoke_insts.at(i * 2 + 1) = inst;
      }
    }
    return invoke_insts;
  };

  // Which blocks should have onMethodExits? Let's ignore infinite loop cases,
  // and do on returns/throws that have no successors.
  const auto& exit_blocks = only_terminal_return_or_throw_blocks(cfg);
  for (cfg::Block* b : exit_blocks) {
    assert(b->succs().empty());
    // The later DedupBlocksPass could deduplicate these calls.
    b->insert_before(b->to_cfg_instruction_iterator(b->get_last_insn()),
                     create_invoke_insts());
  }
  return exit_blocks.size();
}

BlockInfo create_block_info(cfg::Block* block, bool instrument_catches) {
  if (block->num_opcodes() == 0) {
    return {block, BlockType::Empty, {}};
  }

  // TODO: There is a potential register allocation issue when we instrument
  // extremely large number of basic blocks. We've found a case. So, for now,
  // we don't instrument catch blocks with the hope these blocks are cold.
  if (block->is_catch() && !instrument_catches) {
    return {block, BlockType::Catch, {}};
  }

  IRList::iterator insert_pos;
  BlockType type = block->is_catch() ? BlockType::Catch : BlockType::Normal;
  if (block->starts_with_move_result()) {
    insert_pos = get_first_non_move_result_insn(block);
  } else if (block->starts_with_move_exception()) {
    // move-exception must only ever occur as the first instruction of an
    // exception handler; anywhere else is invalid. So, take the next
    // instruction of the move-exception.
    insert_pos = std::next(block->get_first_insn());
    while (insert_pos != block->end() && insert_pos->type != MFLOW_OPCODE) {
      insert_pos = std::next(insert_pos);
    }
    type = type | BlockType::MoveException;
  } else {
    insert_pos = block->get_first_non_param_loading_insn();
  }

  if (insert_pos == block->end()) {
    return {block, BlockType::Useless | type, {}};
  }

  return {block, BlockType::Instrumentable | type, insert_pos};
}

auto get_blocks_to_instrument(const cfg::ControlFlowGraph& cfg,
                              const size_t max_num_blocks,
                              bool instrument_catches) {
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

  // Future work: Pick minimal instrumentation candidates.
  std::vector<BlockInfo> block_info_list;
  block_info_list.reserve(blocks.size());
  BitId id = 0;
  for (cfg::Block* b : blocks) {
    block_info_list.emplace_back(create_block_info(b, instrument_catches));
    auto& info = block_info_list.back();
    if ((info.type & BlockType::Instrumentable) == BlockType::Instrumentable) {
      if (id >= max_num_blocks) {
        return std::make_tuple(std::vector<BlockInfo>{}, BitId(0),
                               true /* too many block */);
      }
      info.bit_id = id++;
    }
  }
  return std::make_tuple(block_info_list, id, false);
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
    IRInstruction* inst = new IRInstruction(OPCODE_OR_INT_LIT16);
    inst->set_literal(static_cast<int16_t>(1ULL << (bit_id % BIT_VECTOR_SIZE)));
    inst->set_src(0, reg_vectors.at(vector_id));
    inst->set_dest(reg_vectors.at(vector_id));
    block->insert_before(block->to_cfg_instruction_iterator(insert_pos), inst);
  }
}

std::vector<const SourceBlock*> gather_source_blocks(const cfg::Block* b) {
  std::vector<const SourceBlock*> ret;
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }
    ret.push_back(mie.src_block.get());
  }
  return ret;
}

MethodInfo instrument_basic_blocks(IRCode& code,
                                   DexMethod* method,
                                   DexMethod* onMethodBegin,
                                   const OnMethodExitMap& onMethodExit_map,
                                   const size_t max_vector_arity,
                                   const size_t method_offset,
                                   const size_t max_num_blocks,
                                   bool instrument_catches) {
  using namespace cfg;

  code.build_cfg(/*editable*/ true);
  ControlFlowGraph& cfg = code.cfg();

  const std::string& before_cfg = show(cfg);

  // Step 1: Get sorted basic blocks to instrument with their information.
  //
  // The blocks are sorted in RPO. We don't instrument entry blocks. If too many
  // blocks, it falls back to empty blocks, which is method tracing.
  std::vector<BlockInfo> blocks;
  size_t num_to_instrument;
  bool too_many_blocks;
  std::tie(blocks, num_to_instrument, too_many_blocks) =
      get_blocks_to_instrument(cfg, max_num_blocks, instrument_catches);

  if (DEBUG_CFG) {
    TRACE(INSTRUMENT, 9, "BEFORE: %s, %s", show_deobfuscated(method).c_str(),
          SHOW(method));
    TRACE(INSTRUMENT, 9, "%s", SHOW(cfg));
  }

  // Step 2: Insert onMethodBegin to track method execution, and bit-vector
  //         allocation code in its method entry point.
  //
  const size_t origin_num_non_entry_blocks = cfg.blocks().size() - 1;
  const size_t num_vectors =
      std::ceil(num_to_instrument / double(BIT_VECTOR_SIZE));
  std::vector<reg_t> reg_vectors;
  reg_t reg_method_offset;
  std::tie(reg_vectors, reg_method_offset) =
      insert_prologue_insts(cfg, onMethodBegin, num_vectors, method_offset);
  const size_t after_prologue_num_non_entry_blocks = cfg.blocks().size() - 1;

  // Step 3: Insert onMethodExit in exit block(s).
  //
  // TODO: What about no exit blocks possibly due to infinite loops? Such case
  // is extremely rare in our apps. In this case, let us do method tracing by
  // instrumenting prologues.
  const size_t num_exit_calls = insert_onMethodExit_calls(
      cfg, reg_vectors, method_offset, reg_method_offset, onMethodExit_map,
      max_vector_arity);

  // Step 4: Insert block coverage update instructions to each blocks.
  //
  insert_block_coverage_computations(blocks, reg_vectors);
  cfg.recompute_registers_size();

  auto count = [&blocks](BlockType type) -> size_t {
    return std::count_if(blocks.begin(), blocks.end(), [type](const auto& i) {
      return (i.type & type) == type;
    });
  };

  MethodInfo info;
  info.method = method;
  info.too_many_blocks = too_many_blocks;
  info.offset = method_offset;
  info.num_non_entry_blocks = cfg.blocks().size() - 1;
  info.num_vectors = num_vectors;
  info.num_exit_calls = num_exit_calls;
  // This CFG hash/signature is to merge data from different build ids.
  info.signature = compute_cfg_signature(blocks);
  info.num_empty_blocks = count(BlockType::Empty);
  info.num_useless_blocks = count(BlockType::Useless);
  info.num_catches = count(BlockType::Catch);
  info.num_instrumented_catches =
      count(BlockType::Catch | BlockType::Instrumentable);
  info.num_instrumented_blocks = num_to_instrument;
  always_assert(count(BlockType::Instrumentable) == num_to_instrument);

  info.bit_id_2_block_id.reserve(num_to_instrument);
  info.bit_id_2_source_blocks.reserve(num_to_instrument);
  for (const auto& i : blocks) {
    if (i.is_instrumentable()) {
      info.bit_id_2_block_id.push_back(i.block->id());
      info.bit_id_2_source_blocks.emplace_back(gather_source_blocks(i.block));
    } else {
      info.rejected_blocks[i.block->id()] = i.type;
    }
  }

  if (DEBUG_CFG) {
    TRACE(INSTRUMENT, 9, "AFTER: %s, %s", show_deobfuscated(method).c_str(),
          SHOW(method));
    TRACE(INSTRUMENT, 9, "%s", SHOW(cfg));
  }

  // Check the post condition:
  //   num_instrumented_blocks == num_non_entry_blocks - num_rejected_blocks
  if (get_instrumented_type(info) != InstrumentedType::MethodOnly &&
      info.bit_id_2_block_id.size() !=
          info.num_non_entry_blocks - info.rejected_blocks.size()) {
    TRACE(INSTRUMENT, 7, "Post condition violation! in %s", SHOW(method));
    TRACE(INSTRUMENT, 7, "- Instrumented type: %d",
          get_instrumented_type(info));
    TRACE(INSTRUMENT, 7, "  %zu != %zu - %zu", info.bit_id_2_block_id.size(),
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

void print_stats(const std::vector<MethodInfo>& instrumented_methods,
                 const size_t max_num_blocks) {
  const size_t total_instrumented = instrumented_methods.size();
  const size_t total_block_instrumented =
      std::count_if(instrumented_methods.begin(), instrumented_methods.end(),
                    [](const MethodInfo& i) { return !i.too_many_blocks; });
  const size_t only_method_instrumented =
      total_instrumented - total_block_instrumented;

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
  TRACE(INSTRUMENT, 4, "Maximum blocks for block instrumentation: %zu",
        max_num_blocks);
  TRACE(INSTRUMENT, 4, "Total instrumented: %zu", total_instrumented);
  TRACE(INSTRUMENT, 4, "- Block + method instrumented: %zu",
        total_block_instrumented);
  TRACE(INSTRUMENT, 4, "- Only method instrumented: %zu",
        only_method_instrumented);

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
          divide(total_bit_vectors, total_block_instrumented).c_str());
  }

  // ----- Instrumented block stats
  TRACE(INSTRUMENT, 4, "Instrumented / actual non-entry block stats:");
  {
    std::map<int, std::pair<size_t /*instrumented*/, size_t /*block*/>> dist;
    size_t total_instrumented_blocks = 0;
    size_t total_non_entry_blocks = 0;
    for (const auto& i : instrumented_methods) {
      if (i.too_many_blocks) {
        ++dist[-1].first;
      } else {
        ++dist[i.num_instrumented_blocks].first;
        total_instrumented_blocks += i.num_instrumented_blocks;
      }
      ++dist[i.num_non_entry_blocks].second;
      total_non_entry_blocks += i.num_non_entry_blocks;
    }
    std::array<size_t, 2> accs = {0, 0};
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %5d blocks: %s | %s", p.first,
            SHOW(print(p.second.first, total_instrumented, accs[0])),
            SHOW(print(p.second.second, total_instrumented, accs[1])));
    }
    TRACE(INSTRUMENT, 4, "Total/average instrumented blocks: %zu, %s",
          total_instrumented_blocks,
          divide(total_instrumented_blocks, total_block_instrumented).c_str());
    TRACE(INSTRUMENT, 4, "Total/average non-entry blocks: %zu, %s",
          total_non_entry_blocks,
          divide(total_non_entry_blocks, total_instrumented).c_str());
  }

  const int total_catches = std::accumulate(
      instrumented_methods.begin(), instrumented_methods.end(), int(0),
      [](int a, auto&& i) { return a + i.num_catches; });
  const int total_instrumented_catches = std::accumulate(
      instrumented_methods.begin(), instrumented_methods.end(), int(0),
      [](int a, auto&& i) { return a + i.num_instrumented_catches; });
  TRACE(INSTRUMENT, 4, "Total catch blocks: %d", total_catches);
  TRACE(INSTRUMENT, 4, "Instrumented catch blocks: %d",
        total_instrumented_catches);
  TRACE(INSTRUMENT, 4, "Ignored catch blocks: %d",
        total_catches - total_instrumented_catches);

  // ----- Instrumented exit block stats
  TRACE(INSTRUMENT, 4, "Instrumented exit block stats:");
  {
    size_t acc = 0;
    size_t total_exits = 0;
    std::map<int /*num_vectors*/, size_t /*num_methods*/> dist;
    TRACE(INSTRUMENT, 4, "No onMethodExit but 1+ non-entry blocks:");
    int k = 0;
    for (const auto& i : instrumented_methods) {
      if (!i.too_many_blocks && i.num_exit_calls == 0 &&
          i.num_non_entry_blocks != 0) {
        TRACE(INSTRUMENT, 4, "- %d: %zu, %s", ++k, i.num_non_entry_blocks,
              show_deobfuscated(i.method).c_str());
      }
      ++dist[i.num_exit_calls];
      total_exits += i.num_exit_calls;
    }
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %4d exits: %s", p.first,
            SHOW(print(p.second, total_instrumented, acc)));
    }
    TRACE(INSTRUMENT, 4, "Total/average instrumented exits: %zu, %s",
          total_exits, divide(total_exits, total_instrumented).c_str());
  }

  // ----- Catch block stats
  TRACE(INSTRUMENT, 4, "Catch block stats:");
  {
    size_t acc = 0;
    size_t total = 0;
    std::map<int, size_t> dist;
    for (const auto& i : instrumented_methods) {
      ++dist[i.num_catches];
      total += i.num_catches;
    }
    for (const auto& p : dist) {
      TRACE(INSTRUMENT, 4, " %4d catches: %s", p.first,
            SHOW(print(p.second, total_instrumented, acc)));
    }
    TRACE(INSTRUMENT, 4, "Total/average catch blocks: %zu, %s", total,
          divide(total, total_instrumented).c_str());
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
          divide(total1, total_block_instrumented).c_str());
    TRACE(INSTRUMENT, 4, "Total/average %s blocks: %zu, %s", name2, total2,
          divide(total2, total_block_instrumented).c_str());
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
    PassManager&,
    const InstrumentPass::Options& options) {
  // I'm too lazy to support sharding in block instrumentation. Future work.
  const size_t NUM_SHARDS = options.num_shards;
  if (NUM_SHARDS != 1 || options.num_stats_per_method != 0) {
    always_assert_log(
        false,
        "[InstrumentPass] error: basic block profiling currently only "
        "supports num_shard = 1 and num_stats_per_method = 0");
  }
  if (options.analysis_method_names.size() != 2) {
    always_assert_log(false,
                      "[InstrumentPass] error: basic block profiling must have "
                      "two analysis methods: [onMethodBegin, onMethodExit]");
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

  const auto& onMethodExit_map =
      build_onMethodExit_map(*analysis_cls, options.analysis_method_names[1]);
  const size_t max_vector_arity = onMethodExit_map.rbegin()->first;
  TRACE(INSTRUMENT, 4, "Max arity for onMethodExit: %zu", max_vector_arity);

  auto cold_start_classes = get_cold_start_classes(cfg);
  TRACE(INSTRUMENT, 7, "Cold start classes: %zu", cold_start_classes.size());

  // This method_offset is used in sMethodStats[] to locate a method profile.
  // We have a small header in the beginning of sMethodStats.
  size_t method_offset = 8;
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

  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    all_methods++;
    if (method == analysis_cls->get_clinit() || method == onMethodBegin) {
      specials++;
      return;
    }

    if (std::any_of(onMethodExit_map.begin(), onMethodExit_map.end(),
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
        code, method, onMethodBegin, onMethodExit_map, max_vector_arity,
        method_offset, max_num_blocks, options.instrument_catches));

    const auto& method_info = instrumented_methods.back();
    if (method_info.too_many_blocks) {
      TRACE(INSTRUMENT, 7, "Too many blocks: %s",
            SHOW(show_deobfuscated(method)));
    } else {
      block_instrumented++;
    }

    // Update method offset for next method. 2 shorts are for method stats.
    method_offset += 2 + method_info.num_vectors;
  });

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

  write_metadata(cfg, options.metadata_file_name, instrumented_methods);

  print_stats(instrumented_methods, max_num_blocks);

  TRACE(INSTRUMENT, 4, "Instrumentation selection stats:");
  TRACE(INSTRUMENT, 4, "- All methods: %d", all_methods);
  TRACE(INSTRUMENT, 4, "- Eligible methods: %d", eligibles);
  TRACE(INSTRUMENT, 4, "  Uninstrumentable methods: %d", specials);
  TRACE(INSTRUMENT, 4, "  Non-root methods: %d", non_root_store_methods);
  TRACE(INSTRUMENT, 4, "- Explicitly selected:");
  TRACE(INSTRUMENT, 4, "  Allow listed: %d", picked_by_allowlist);
  TRACE(INSTRUMENT, 4, "  Cold start: %d", picked_by_cs);
  TRACE(INSTRUMENT, 4, "- Explicitly rejected:");
  TRACE(INSTRUMENT, 4, "  Not in allow or cold start set: %d", rejected);
  TRACE(INSTRUMENT, 4, "  Block listed: %d", blocklisted);
}
