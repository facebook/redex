/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockInstrument.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "MethodReference.h"
#include "Show.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace instrument;

namespace {

constexpr bool DEBUG_CFG = false;
constexpr bool CONST_PROP = false;
constexpr size_t BIT_VECTOR_SIZE = 16;
constexpr size_t MAX_BLOCKS = 0;

using OnMethodExitMap =
    std::map<size_t, // arity of vector arguments (excluding `int offset`)
             std::pair<DexMethod*, // onMethodExit
                       DexMethod*>>; // onMethodExit_Epilogue

struct MethodInfo {
  struct Stats {
    size_t num_blocks = 0;
    size_t num_regs = 0;
    size_t num_exits = 0;
  };

  const DexMethod* method = nullptr;
  // All eligible methods are at least method instrumented. This indicates
  // whether this method is also block instrumented.
  bool is_block_instrumented = false;
  // The offset is used in `short[] DynamicAnalysis.sMethodStats`. The first two
  // shorts are for method method profiling, and short[num_vectors] are for
  // block coverages.
  size_t offset = 0;
  size_t num_non_entry_blocks = 0;
  size_t num_vectors = 0;
  size_t num_exit_calls = 0;
  uint64_t signature = 0;

  Stats before;
  Stats after;
};

uint64_t compute_cfg_signature(const std::map<size_t, cfg::Block*> blocks) {
  // Blocks should be sorted in a deterministic way like a RPO.
  // Encode block shapes with opcodes lists per block.
  std::ostringstream serialized;
  for (const auto& pair : blocks) {
    const cfg::Block* b = pair.second;
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
    cfg::ControlFlowGraph& cfg, bool log = false) {

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

OnMethodExitMap build_onMethodExit_map(const DexClass& cls,
                                       const std::string& onMethodExit_name) {
  const std::string epilogue_name = onMethodExit_name + "_Epilogue";
  std::map<size_t, std::pair<DexMethod*, DexMethod*>> onMethodExit_map;
  for (const auto& m : cls.get_dmethods()) {
    const auto& name = m->get_name()->str();
    if (onMethodExit_name != name && epilogue_name != name) {
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

    auto& pair = onMethodExit_map[args->size() - 1];
    if (epilogue_name == name) {
      pair.second = m;
    } else {
      pair.first = m;
    }
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

  // For all non-zero arities, both onMethodExit/_Epilogue must exist.
  if (std::any_of(
          begin(onMethodExit_map), end(onMethodExit_map), [](const auto& kv) {
            return kv.first != 0 &&
                   (kv.second.first == nullptr || kv.second.second == nullptr);
          })) {
    std::stringstream ss;
    for (const auto& kv : onMethodExit_map) {
      ss << " arity: " << kv.first
         << ", onMethodExit: " << (kv.second.first ? "T" : "F")
         << ", onMethodExit_Epilogue: " << (kv.second.second ? "T" : "F")
         << std::endl;
    }
    always_assert_log(
        false,
        "[InstrumentPass] error: there must be a pair of onMethodExit "
        "and onMethodExit_Epilogue for each overloaded type, except "
        "for zero arity:\n%s",
        ss.str().c_str());
  }

  return onMethodExit_map;
}

auto insert_allocation_insts(cfg::ControlFlowGraph& cfg,
                             const size_t num_vectors, // May be zero
                             const size_t method_offset) {
  std::vector<reg_t> reg_vectors(num_vectors);
  // Create instructions to allocate a set of 16-bit bit vectors.
  // +1 is for holding method offset in DynamicAnalysis.sBlockStats[].
  std::vector<IRInstruction*> const_insts(num_vectors + 1);
  for (size_t i = 0; i < num_vectors; ++i) {
    const_insts.at(i) = new IRInstruction(OPCODE_CONST);
    const_insts.at(i)->set_literal(0);
    reg_vectors.at(i) = cfg.allocate_temp();
    const_insts.at(i)->set_dest(reg_vectors.at(i));
  }

  // We also allocate a register that holds the method offset, which is used
  // to call onMethodExit. Let's create once in the entry block, and later all
  // exit blocks will use it.
  IRInstruction* method_offset_inst = new IRInstruction(OPCODE_CONST);
  method_offset_inst->set_literal(method_offset);
  const reg_t reg_method_offset = cfg.allocate_temp();
  method_offset_inst->set_dest(reg_method_offset);
  const_insts.at(num_vectors) = method_offset_inst;

  // Insert all OPCODE_CONSTs to the entry block (right after param loading).
  cfg.entry_block()->insert_before(
      cfg.entry_block()->to_cfg_instruction_iterator(
          cfg.entry_block()->get_first_non_param_loading_insn()),
      const_insts);

  return std::make_tuple(reg_vectors, reg_method_offset);
}

size_t insert_onMethodExit_calls(
    cfg::ControlFlowGraph& cfg,
    const std::vector<reg_t>& reg_vectors, // May be empty
    const size_t method_offset,
    const reg_t reg_method_offset,
    const std::map<size_t, std::pair<DexMethod*, DexMethod*>>& onMethodExit_map,
    const size_t max_vector_arity) {
  // When a method exits, we call onMethodExit to pass all vectors to record.
  // onMethodExit is overloaded to some degrees (e.g., up to 5 vectors). If
  // number of vectors > 5, generate one or more onMethodExit_Epilogue calls.
  //
  // Even if reg_vectors is emptry (methods with a single entry block), we still
  // call 'onMethodExit(int offset)' to track method execution.
  const size_t num_vectors = reg_vectors.size();
  const size_t empty = num_vectors == 0 ? 0 : 1;
  const size_t num_invokes =
      std::max(1., std::ceil(double(num_vectors) / double(max_vector_arity)));
  const size_t remainder = (num_vectors % max_vector_arity) == 0
                               ? max_vector_arity * empty
                               : (num_vectors % max_vector_arity);

  auto create_invoke_insts = [&]() -> auto {
    // This code works in case of num_invokes == 1.
    std::vector<IRInstruction*> invoke_insts(num_invokes * 2 - 1);
    size_t offset = method_offset;
    for (size_t i = 0; i < num_invokes; ++i) {
      const size_t arity =
          (i != num_invokes - 1) ? max_vector_arity * empty : remainder;

      IRInstruction* inst = new IRInstruction(OPCODE_INVOKE_STATIC);
      const auto& pair = onMethodExit_map.at(arity);
      // onMethodExit followed by zero or more onMethodExit_Epilogue.
      inst->set_method(i == 0 ? pair.first : pair.second);
      inst->set_srcs_size(arity + 1);
      inst->set_src(0, reg_method_offset);
      for (size_t j = 0; j < arity; ++j) {
        inst->set_src(j + 1, reg_vectors[max_vector_arity * i + j]);
      }
      invoke_insts.at(i * 2) = inst;

      if (i != num_invokes - 1) {
        inst = new IRInstruction(OPCODE_CONST);
        // Move forward the offset. Note that the first onMethodExit writes
        // method profiling in the first two elements. So, we have +2 here.
        offset += max_vector_arity + (i == 0 ? 2 : 0);
        inst->set_literal(offset);
        inst->set_dest(reg_method_offset);
        invoke_insts.at(i * 2 + 1) = inst;
      }
    }
    return invoke_insts;
  };

  // Which blocks should have onMethodExits? Let's ignore infinite loop cases,
  // and do on returns/throws that have no successors.
  const auto& exit_blocks = only_terminal_return_or_throw_blocks(cfg, true);
  for (cfg::Block* b : exit_blocks) {
    assert(b->succs().empty());
    // The later DedupBlocksPass could deduplicate these calls.
    b->insert_before(b->to_cfg_instruction_iterator(b->get_last_insn()),
                     create_invoke_insts());
  }
  return exit_blocks.size();
}

auto get_blocks_to_instrument(const cfg::ControlFlowGraph& cfg) {
  auto blocks = graph::postorder_sort<cfg::GraphInterface>(cfg);

  // We don't need to instrument entry block obviously.
  assert(blocks.back() == cfg.entry_block());
  blocks.pop_back();

  // Convert to reverse postorder (RPO).
  std::reverse(blocks.begin(), blocks.end());

  // Create a map of bit id to Block*.
  std::map<size_t /*bit_id*/, cfg::Block*> id_block_map;

  // Future work: Pick minimal instrumentation candidates.
  size_t id = 0;
  for (cfg::Block* b : blocks) {
    id_block_map[id++] = b;
  }
  return id_block_map;
}

MethodInfo instrument_basic_blocks(IRCode& code,
                                   DexMethod* method,
                                   const OnMethodExitMap& onMethodExit_map,
                                   const size_t max_vector_arity,
                                   const size_t method_offset) {
  using namespace cfg;

  auto fill_stats = [](MethodInfo::Stats& stats, ControlFlowGraph& cfg) {
    stats.num_blocks = cfg.blocks().size();
    stats.num_regs = cfg.get_registers_size();
    stats.num_exits = only_terminal_return_or_throw_blocks(cfg).size();
  };

  code.build_cfg(/*editable*/ true);
  ControlFlowGraph& cfg = code.cfg();

  // Step 1: Get sorted basic blocks to instrument.
  //
  // The blocks are sorted in RPO. We don't instrument entry blocks. If too many
  // blocks, it falls back to empty blocks, which is method tracing.
  std::map<size_t /*bit id*/, Block*> block_map{};
  always_assert(cfg.blocks().size() > 0);
  size_t num_non_entry_blocks = cfg.blocks().size() - 1;
  const bool reject_block_instrument = (num_non_entry_blocks >= MAX_BLOCKS);
  if (!reject_block_instrument) {
    // Get candidate blocks to be instrumented. Some blocks may be empty, have
    // no effective opcodes, or catch blocks.
    block_map = get_blocks_to_instrument(cfg);
  }
  const size_t num_blocks_to_instrument = block_map.size();

  if (DEBUG_CFG) {
    TRACE(INSTRUMENT, 9, "BEFORE: %s, %s", show_deobfuscated(method).c_str(),
          SHOW(method));
    TRACE(INSTRUMENT, 9, "%s", SHOW(cfg));
  }

  const size_t num_vectors =
      std::ceil(num_blocks_to_instrument / double(BIT_VECTOR_SIZE));
  if (!reject_block_instrument) {
    TRACE(INSTRUMENT, 9, "[%s] non-entry blocks: %zu, bit-vectors: %zu",
          SHOW(method->get_name()), num_non_entry_blocks, num_vectors);

  } else {
    TRACE(INSTRUMENT, 9, "[%s] non-entry blocks: %zu", SHOW(method->get_name()),
          num_non_entry_blocks);
  }

  MethodInfo info;
  info.method = method;
  // This CFG hash/signature is to merge data from different build ids.
  info.signature = compute_cfg_signature(block_map);
  info.offset = method_offset;
  info.num_non_entry_blocks = num_non_entry_blocks;
  info.num_vectors = num_vectors;
  info.is_block_instrumented = !reject_block_instrument;
  fill_stats(info.before, cfg);

  // Step 2: Insert bit-vector allocation code in the block entry point.
  //
  std::vector<reg_t> reg_vectors;
  reg_t reg_method_offset;
  std::tie(reg_vectors, reg_method_offset) =
      insert_allocation_insts(cfg, num_vectors, method_offset);

  // Step 3: Insert onMethodExit in exit block(s).
  //
  info.num_exit_calls = insert_onMethodExit_calls(
      cfg, reg_vectors, method_offset, reg_method_offset, onMethodExit_map,
      max_vector_arity);

  // Not sure whether inserting onMethodExits would change CFG shape?
  cfg.recompute_registers_size();
  fill_stats(info.after, cfg);

  // Step 4: Insert block coverage update instructions to each blocks.
  //
  // TODO: Next diff

  if (DEBUG_CFG) {
    TRACE(INSTRUMENT, 9, "AFTER: %s, %s", show_deobfuscated(method).c_str(),
          SHOW(method));
    TRACE(INSTRUMENT, 9, "%s", SHOW(cfg));
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
// This instrumentation subsumes the method tracing. We currently don't
// instrument methods with large number of basic blocks. In this case, they are
// only instrumented for method tracing with onMethodExit.
//------------------------------------------------------------------------------
void BlockInstrumentHelper::do_basic_block_tracing(
    DexClass* analysis_cls,
    DexStoresVector& stores,
    ConfigFiles& cfg,
    PassManager& pm,
    const InstrumentPass::Options& options) {
  // I'm too lazy to support sharding in block instrumentation. Future work.
  const size_t NUM_SHARDS = options.num_shards;
  if (NUM_SHARDS != 1 || options.num_stats_per_method != 0) {
    std::cerr << "[InstrumentPass] error: basic block profiling currently only "
                 "supports num_shard = 1 and num_stats_per_method = 0"
              << std::endl;
    exit(1);
  }

  // Even so, we need to update sharded arrays with 1 for the Java-side code.
  const auto& array_fields = InstrumentPass::patch_sharded_arrays(
      analysis_cls, NUM_SHARDS,
      // However, because we have only one shard and don't clone onMethodExits,
      // we keep the original name. It actually fools patch_sharded_arrays.
      {{1, InstrumentPass::STATS_FIELD_NAME}});
  always_assert(array_fields.size() == NUM_SHARDS);

  const auto& onMethodExit_map =
      build_onMethodExit_map(*analysis_cls, options.analysis_method_name);
  const size_t max_vector_arity = onMethodExit_map.rbegin()->first;

  auto cold_start_classes = get_cold_start_classes(cfg);
  TRACE(INSTRUMENT, 7, "Cold start classes: %zu", cold_start_classes.size());

  // This method_offset is used in sMethodStats[] to locate a method profile.
  size_t method_offset = 0;
  std::vector<MethodInfo> instrumented_methods;

  int all_methods = 0;
  int eligibles = 0;
  int specials = 0;
  int picked_by_cs = 0;
  int picked_by_allowlist = 0;
  int blocklisted = 0;
  int rejected = 0;
  int block_instrumented = 0;

  auto scope = build_class_scope(stores);
  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    all_methods++;
    if (method == analysis_cls->get_clinit()) {
      specials++;
      return;
    }

    if (std::any_of(onMethodExit_map.begin(), onMethodExit_map.end(),
                    [&](const auto& e) {
                      return e.second.first == method ||
                             e.second.second == method;
                    })) {
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
        code, method, onMethodExit_map, max_vector_arity, method_offset));

    const auto& method_info = instrumented_methods.back();
    if (!method_info.is_block_instrumented) {
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

  // TODO: Write metadata
  // TODO: Print stats

  TRACE(INSTRUMENT, 4, "Instrumentation selection stats:");
  TRACE(INSTRUMENT, 4, "- All methods: %d", all_methods);
  TRACE(INSTRUMENT, 4, "- Eligible methods: %d", eligibles);
  TRACE(INSTRUMENT, 4, "  Uninstrumentable methods: %d", specials);
  TRACE(INSTRUMENT, 4, "- Explicitly selected:");
  TRACE(INSTRUMENT, 4, "  Allow listed: %d", picked_by_allowlist);
  TRACE(INSTRUMENT, 4, "  Cold start: %d", picked_by_cs);
  TRACE(INSTRUMENT, 4, "- Explicitly rejected:");
  TRACE(INSTRUMENT, 4, "  Not in allow or cold start set: %d", rejected);
  TRACE(INSTRUMENT, 4, "  Block listed: %d", blocklisted);
  TRACE(INSTRUMENT, 4, "- Instrumented: %zu", instrumented_methods.size());
  TRACE(INSTRUMENT, 4, "  Only method instrumented: %d", block_instrumented);
}
