/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockInstrument.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "MethodReference.h"
#include "Show.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void write_basic_block_index_file(
    const std::string& file_name,
    const std::map<int, std::pair<std::string, int>>& id_name_map) {
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  for (const auto& p : id_name_map) {
    ofs << p.first << "," << p.second.first << "," << p.second.second
        << std::endl;
  }
  TRACE(INSTRUMENT, 2, "Index file was written to: %s", SHOW(file_name));
}

std::unordered_map<int, /*num of arguments*/ DexMethod*> build_onMethodExit_map(
    const DexClass& cls, const std::string& onMethodExit_name) {
  std::unordered_map<int, DexMethod*> onMethodExit_map;
  for (const auto& dm : cls.get_dmethods()) {
    if (onMethodExit_name == dm->get_name()->c_str()) {
      auto const v =
          std::next(dm->get_proto()->get_args()->get_type_list().begin(), 1);
      if (*v == DexType::make_type("[S")) {
        // General case: void onMethodExit(int offset, short[] bbVector)
        onMethodExit_map.emplace(1, dm);
      } else {
        // Specific cases: void onMethodExit(int offset, short vec1, ..., vecN)
        onMethodExit_map.emplace(dm->get_proto()->get_args()->size(), dm);
      }
    }
  }

  if (!onMethodExit_map.empty()) {
    return onMethodExit_map;
  }

  std::cerr << "[InstrumentPass] error: cannot find " << onMethodExit_name
            << " in " << show(cls) << std::endl;
  for (const auto& m : cls.get_dmethods()) {
    std::cerr << " " << show(m) << std::endl;
  }
  exit(1);
}

// Find if the current insertion point lies within a try_start - try_end block.
// If it does, return its corresponding catch block.
MethodItemEntry* find_try_block(IRCode* code,
                                const IRList::iterator& insert_point) {
  for (auto mie_try = insert_point; mie_try != code->begin(); --mie_try) {
    if (mie_try->type == MFLOW_TRY && mie_try->tentry->type == TRY_END) {
      return nullptr;
    } else if (mie_try->type == MFLOW_TRY &&
               mie_try->tentry->type == TRY_START) {
      return mie_try->tentry->catch_start;
    }
  }
  return nullptr;
}

void insert_try_start_instr(IRCode* code,
                            const IRList::iterator& insert_point,
                            MethodItemEntry* catch_block) {
  if (catch_block) {
    auto try_start = new MethodItemEntry(TRY_START, catch_block);
    code->insert_before(insert_point, *try_start);
  }
}

void insert_try_end_instr(IRCode* code,
                          const IRList::iterator& insert_point,
                          MethodItemEntry* catch_block) {
  if (catch_block) {
    auto try_end = new MethodItemEntry(TRY_END, catch_block);
    code->insert_before(insert_point, *try_end);
  }
}

void insert_invoke_instr(IRCode* code,
                         const IRList::iterator& insert_point,
                         IRInstruction* method_id_inst,
                         IRInstruction* invoke_inst) {
  // When inserting an INVOKE instruction within a try_catch block, in
  // order to prevent adding an extra throw edge to the CFG, we split the
  // block into two. We insert a TRY_END instruction before our
  // instrumentation INVOKE call and a TRY_START after.
  auto catch_block = find_try_block(code, insert_point);

  insert_try_end_instr(code, insert_point, catch_block);
  code->insert_before(code->insert_before(insert_point, invoke_inst),
                      method_id_inst);
  insert_try_start_instr(code, insert_point, catch_block);
}

// TODO(minjang): We could simply utilize already existing analysis methods
// instead of making an array. For an instance, to handle 42 bit vectors, We
// could call 8 times of onMethodExitBB(SSSSS) and 1 time of onMethodExitBB(SS).
//
// ---
// When the number of bit vectors for a method is more than 5, they are added to
// an array, and the array is passed to onMethodExitBB() along with method id.
//
// Algorithm:
//  Bit vectors: v_0, v_1, v_2, v_3, v_4, v_5
//  num_vectors: 6
//  Initialize size of new array: CONST <v_array>, 6
//  Initialize a new empty array: NEW_ARRAY <v_array>, [S
//  For each bit vector <v_i>:
//   Initialize index:     CONST <v_pos>, [position]
//   Add to array:         APUT_OBJECT <v_i>, <v_array> , <v_pos>
//  Finally, call analysis_method:
//   Initialize method id: CONST <v_mId>, [method_id]
//   Actual call:          INVOKE_STATIC <v_mId>, <v_array>
//
// Example:
//  CONST v59, 6
//  NEW_ARRAY v59, [S
//  IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v59
//  CONST v60, 0
//  APUT v52, v59, v60
//  CONST v61, 1
//  APUT v53, v59, v61
//  CONST v62, 2
//  APUT v54, v59, v62
//  CONST v63, 3
//  APUT v55, v59, v63
//  CONST v64, 4
//  APUT v56, v59, v64
//  CONST v65, 5
//  APUT v57, v59, v65
//  CONST v66, 6171
//  INVOKE_STATIC v66, v59, Lcom/foo/Analysis;.onMethodExitBB:(I[S)V
void insert_invoke_static_array_arg(IRCode* code,
                                    size_t method_id,
                                    DexMethod* method_onMethodExit,
                                    const std::vector<reg_t>& reg_bb_vector,
                                    const IRList::iterator& insert_point) {
  // If num_vectors >5, create array of all bit vectors. Add as argument to
  // invoke call.
  size_t num_vectors = reg_bb_vector.size();
  // Create new array.
  IRInstruction* array_size_inst = new IRInstruction(OPCODE_CONST);
  array_size_inst->set_literal(num_vectors);
  const auto array_dest = code->allocate_temp();
  array_size_inst->set_dest(array_dest);

  IRInstruction* new_array_insn = new IRInstruction(OPCODE_NEW_ARRAY);
  new_array_insn->set_type(type::make_array_type(type::_short()));
  new_array_insn->set_srcs_size(1);
  new_array_insn->set_src(0, array_dest);

  IRInstruction* move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  move_result_pseudo->set_dest(array_dest);

  auto catch_block = find_try_block(code, insert_point);

  insert_try_end_instr(code, insert_point, catch_block);
  code->insert_before(
      code->insert_before(code->insert_before(insert_point, move_result_pseudo),
                          new_array_insn),
      array_size_inst);
  insert_try_start_instr(code, insert_point, catch_block);

  // Set of instructions to add bit vectors to newly created array.
  std::vector<IRInstruction*> index_inst(num_vectors);
  std::vector<IRInstruction*> aput_inst(num_vectors);
  std::vector<reg_t> index_reg(num_vectors);

  catch_block = find_try_block(code, insert_point);

  insert_try_end_instr(code, insert_point, catch_block);
  for (size_t i = 0; i < num_vectors; i++) {
    // Initialize index where this element is to be inserted.
    index_inst[i] = new IRInstruction(OPCODE_CONST);
    index_inst[i]->set_literal(i);
    index_reg[i] = code->allocate_temp();
    index_inst[i]->set_dest(index_reg[i]);

    // APUT instruction adds the bit vector to the array.
    aput_inst[i] = new IRInstruction(OPCODE_APUT_SHORT);
    aput_inst[i]->set_srcs_size(3);
    aput_inst[i]->set_src(0, reg_bb_vector[i]);
    aput_inst[i]->set_src(1, array_dest);
    aput_inst[i]->set_src(2, index_reg[i]);

    code->insert_before(code->insert_before(insert_point, aput_inst[i]),
                        index_inst[i]);
  }
  insert_try_start_instr(code, insert_point, catch_block);

  // Finally, make the INVOKE_STATIC call to analysis function.
  // Argument 1: Method ID, Argument 2: Array of bit vectors.
  IRInstruction* method_id_inst = new IRInstruction(OPCODE_CONST);
  method_id_inst->set_literal(method_id);
  auto reg_method_id = code->allocate_temp();
  method_id_inst->set_dest(reg_method_id);

  IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_inst->set_method(method_onMethodExit);
  invoke_inst->set_srcs_size(2);
  invoke_inst->set_src(0, reg_method_id);
  invoke_inst->set_src(1, array_dest);

  insert_invoke_instr(code, insert_point, method_id_inst, invoke_inst);
}

void insert_onMethodExit_call(IRCode* code,
                              size_t method_id,
                              DexMethod* method_onMethodExit,
                              const std::vector<reg_t>& reg_bb_vector) {
  for (auto mie = code->begin(); mie != code->end(); ++mie) {
    if (mie->type != MFLOW_OPCODE ||
        (mie->insn->opcode() != OPCODE_RETURN &&
         mie->insn->opcode() != OPCODE_RETURN_OBJECT &&
         mie->insn->opcode() != OPCODE_RETURN_VOID)) {
      continue;
    }

    IRInstruction* method_id_inst = new IRInstruction(OPCODE_CONST);
    method_id_inst->set_literal(method_id);
    auto reg_method_id = code->allocate_temp();
    method_id_inst->set_dest(reg_method_id);

    assert(!reg_bb_vector.empty());
    if (reg_bb_vector.size() <= 5) {
      IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
      // Method to be invoked depends on the number of vectors in current
      // method. The '1' is added to count first argument i.e. method_id.
      invoke_inst->set_method(method_onMethodExit);
      invoke_inst->set_srcs_size(reg_bb_vector.size() + 1);
      invoke_inst->set_src(0, reg_method_id);
      for (size_t reg_index = 0; reg_index < reg_bb_vector.size();
           ++reg_index) {
        invoke_inst->set_src(reg_index + 1, reg_bb_vector[reg_index]);
      }
      insert_invoke_instr(code, mie, method_id_inst, invoke_inst);
    } else {
      insert_invoke_static_array_arg(code, method_id, method_onMethodExit,
                                     reg_bb_vector, mie);
    }
  }
}

IRList::iterator find_or_insn_insert_point(cfg::Block* block) {
  // After every invoke instruction, the value returned from the function is
  // moved to a register. The instruction used to move depends on the type of
  // return value. Our instrumentation should skip over all these move_result
  // instructions.
  return std::find_if_not(
      block->begin(), block->end(), [&](const MethodItemEntry& mie) {
        auto mie_op = mie.insn->opcode();
        return (mie.type == MFLOW_OPCODE &&
                (opcode::is_internal(mie_op) || mie_op == OPCODE_MOVE ||
                 mie_op == OPCODE_MOVE_WIDE ||
                 mie_op == OPCODE_MOVE_EXCEPTION ||
                 mie_op == OPCODE_MOVE_OBJECT || mie_op == OPCODE_MOVE_RESULT ||
                 mie_op == OPCODE_MOVE_RESULT_OBJECT ||
                 mie_op == OPCODE_MOVE_RESULT_WIDE)) ||
               (mie.type == MFLOW_TRY &&
                (mie.tentry->type == TRY_END || mie.tentry->type == TRY_START));
        //
        // TODO(minjang): Fix a bug! Should pass MFLOW_TARGET as well.
        //
      });
}

// For every basic block, add an instruction to calculate:
//   bit_vector[n] OR (1 << block_id).
//
// We use 16-bit short vectors to capture all the basic blocks in a method. We
// reserve the MSB of each 16-bit vector as the end marker. This end marker is
// used to distinguish multiple bit vectors. The set MSB indicates continuation:
// read the following vector(s) again until we see the unset MSB. We actually
// use 15 bits per vector.
int instrument_basic_blocks(
    IRCode* code,
    DexMethod* method,
    const std::unordered_map<int, DexMethod*>& onMethodExit_map,
    size_t method_id,
    int& num_all_bbs,
    int& num_blocks_instrumented,
    int& num_instrumented_methods,
    std::map<int, std::pair<std::string, int>>& method_id_name_map,
    std::map<size_t, int>& bb_vector_stat) {
  assert(code != nullptr);

  code->build_cfg(/*editable*/ false);
  const auto& blocks = code->cfg().blocks();

  // Compute the number of necessary vectors to hold all blocks data. We use
  // only 15 bits for each vector.
  const size_t num_vectors = (blocks.size() + 15 - 1) / 15;

  TRACE(INSTRUMENT, 7, "[%s] %zu blocks with %zu 16-bit vectors",
        SHOW(method->get_name()), blocks.size(), num_vectors);

  num_all_bbs += blocks.size();

  // Allocate short[num_vectors] in the entry block.
  std::vector<reg_t> reg_vectors(num_vectors);
  std::vector<IRInstruction*> const_insts(num_vectors);
  for (size_t i = 0; i < num_vectors; ++i) {
    const_insts.at(i) = new IRInstruction(OPCODE_CONST);
    // Set the end marker at MSB. 0 at the last vector.
    const_insts.at(i)->set_literal(i == num_vectors - 1 ? 0 : -32768);
    reg_vectors.at(i) = code->allocate_temp();
    const_insts.at(i)->set_dest(reg_vectors.at(i));
  }

  // Before instrumenting blocks, we add an INVOKE_STATIC onMethodExit to send
  // the bit vectors for logging, at the exit point. Note that this may alter
  // the CFG. So, this needs to be done before actual instrumentation.
  //
  // The INVOKE call takes (num_vectors + 1) arguments: Method ID (actually,
  // the short array offset) and bit vectors * n. If num_vectors > 5, we call
  // the general version of onMethodExit, onMethodExit(int offset, short[]).
  size_t index_to_method = (num_vectors > 5) ? 1 : num_vectors + 1;
  ++bb_vector_stat[num_vectors];
  assert(onMethodExit_map.count(index_to_method));
  insert_onMethodExit_call(code, method_id,
                           onMethodExit_map.at(index_to_method), reg_vectors);

  for (cfg::Block* block : blocks) {
    const size_t block_vector_index = block->id() / 15;
    // Insert 'block_bit_vector |= 1 << block->id()'.
    IRInstruction* or_inst = new IRInstruction(OPCODE_OR_INT_LIT16);
    or_inst->set_literal(static_cast<int16_t>(1ULL << (block->id() % 15)));
    or_inst->set_src(0, reg_vectors.at(block_vector_index));
    or_inst->set_dest(reg_vectors.at(block_vector_index));

    // Find where to insert the newly created instruction block.
    auto insert_point = find_or_insn_insert_point(block);

    // We do not instrument a basic block if:
    // 1. It only has internal or MOVE instructions.
    // 2. BB has no opcodes.
    if (insert_point == block->end() || block->num_opcodes() < 1) {
      TRACE(INSTRUMENT, 7, "No instrumentation to block: %s",
            SHOW(show(method) + std::to_string(block->id())));
      continue;
    }
    num_blocks_instrumented++;
    code->insert_before(insert_point, or_inst);
  }

  // We use intentionally obfuscated name to guarantee the uniqueness.
  const auto& method_name = show(method);
  assert(!method_id_name_map.count(method_id));
  method_id_name_map.emplace(method_id,
                             std::make_pair(method_name, blocks.size()));

  // Find a point at the beginning of the method to insert the init
  // instruction. This initializes bit vector registers to 0.
  auto insert_point_init = std::find_if_not(
      code->begin(), code->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });

  for (size_t reg_index = 0; reg_index < num_vectors; ++reg_index) {
    code->insert_before(insert_point_init, const_insts.at(reg_index));
  }

  TRACE(INSTRUMENT, 7, "Id: %zu Method: %s", method_id, SHOW(method_name));
  TRACE(INSTRUMENT, 7, "After Instrumentation Full:\n %s", SHOW(code));

  method_id += num_vectors;
  num_instrumented_methods++;
  return method_id;
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

////////////////////////////////////////////////////////////////////////////////
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
////////////////////////////////////////////////////////////////////////////////
void BlockInstrumentHelper::do_basic_block_tracing(
    DexClass* analysis_cls,
    DexStoresVector& stores,
    ConfigFiles& cfg,
    PassManager& pm,
    const InstrumentPass::Options& options) {
  const auto& onMethodExit_map =
      build_onMethodExit_map(*analysis_cls, options.analysis_method_name);

  auto cold_start_classes = get_cold_start_classes(cfg);
  TRACE(INSTRUMENT, 7, "Cold start classes: %zu", cold_start_classes.size());

  size_t method_index = 1;
  int num_all_bbs = 0;
  int num_instrumented_bbs = 0;
  int num_instrumented_methods = 0;

  int eligibles = 0;
  int specials = 0;
  int picked_by_cs = 0;
  int picked_by_whitelist = 0;
  int blacklisted = 0;
  int rejected = 0;
  int candidates = 0;

  std::map<int /*id*/, std::pair<std::string /*name*/, int /*num_BBs*/>>
      method_id_name_map;
  std::map<size_t /*num_vectors*/, int /*count*/> bb_vector_stat;
  auto scope = build_class_scope(stores);
  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method == analysis_cls->get_clinit()) {
      specials++;
      return;
    }
    if (std::any_of(onMethodExit_map.begin(),
                    onMethodExit_map.end(),
                    [&](const auto& e) { return e.second == method; })) {
      specials++;
      return;
    }

    eligibles++;
    if (!options.whitelist.empty() || options.only_cold_start_class) {
      if (InstrumentPass::is_included(method, options.whitelist)) {
        picked_by_whitelist++;
      } else if (InstrumentPass::is_included(method, cold_start_classes)) {
        picked_by_cs++;
      } else {
        rejected++;
        return;
      }
    }

    // Blacklist has priority over whitelist or cold start list.
    if (InstrumentPass::is_included(method, options.blacklist)) {
      blacklisted++;
      return;
    }

    candidates++;
    TRACE(INSTRUMENT, 9, "Candidate: %s", SHOW(method));

    // TODO: next diff will fix it.
    if (0) {
      method_index = instrument_basic_blocks(
          &code, method, onMethodExit_map, method_index, num_all_bbs,
          num_instrumented_bbs, num_instrumented_methods, method_id_name_map,
          bb_vector_stat);
    }
  });
  InstrumentPass::patch_array_size(analysis_cls, "sBasicBlockStats",
                                   method_index);

  write_basic_block_index_file(cfg.metafile(options.metadata_file_name),
                               method_id_name_map);

  double cumulative = 0.;
  TRACE(INSTRUMENT, 4, "BB vector stats:");
  for (const auto& p : bb_vector_stat) {
    double percent = p.second * 100. / (double)num_instrumented_methods;
    cumulative += percent;
    TRACE(INSTRUMENT, 4, " %3zu bit vectors: %6d (%6.3lf%%, %6.3lf%%)", p.first,
          p.second, percent, cumulative);
  }

  TRACE(INSTRUMENT, 4, "Instrumentation candidates selection stats:");
  TRACE(INSTRUMENT, 4, "- All eligible methods: %d", eligibles);
  TRACE(INSTRUMENT, 4, "  (Special uninstrumentable methods: %d)", specials);
  TRACE(INSTRUMENT, 4, "- Not selected: %d", rejected);
  TRACE(INSTRUMENT, 4, "- Selected by whitelist: %d", picked_by_whitelist);
  TRACE(INSTRUMENT, 4, "- Selected by cold start set: %d", picked_by_cs);
  TRACE(INSTRUMENT, 4, "  (But rejected by blacklist: %d)", blacklisted);
  TRACE(INSTRUMENT, 4, "- Total candidates: %d", candidates);
}
