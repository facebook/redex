/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Instrument.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Match.h"
#include "Show.h"
#include "Walkers.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * This pass performs instrumentation for dynamic (runtime) analysis.
 *
 * Analysis code, which should be a static public method, is written in Java.
 * Its class and method names are specified in the config. This pass then
 * inserts the method to points of interest. For a starting example, we
 * implement the "onMethodBegin" instrumentation.
 */
namespace {

static bool debug = false;

// For example, say that "Lcom/facebook/debug/" is in the set. We match either
// "^Lcom/facebook/debug/*" or "^Lcom/facebook/debug;".
bool match_class_name(std::string cls_name,
                      const std::unordered_set<std::string>& set) {
  always_assert(cls_name.back() == ';');
  cls_name.back() = '/';
  size_t pos = cls_name.find('/', 0);
  while (pos != std::string::npos) {
    if (set.count(cls_name.substr(0, pos + 1))) {
      return true;
    }
    pos = cls_name.find('/', pos + 1);
  }
  return false;
}

// Check for inclusion in whitelist or blacklist of methods/classes.
bool is_included(const std::string& method_name,
                 const std::string& cls_name,
                 const std::unordered_set<std::string>& set) {
  if (match_class_name(cls_name, set)) {
    return true;
  }
  // Check for method by its full name(Class_Name;Method_Name).
  return set.count(cls_name + method_name);
}

std::unordered_map<int /* Number of arguments*/, DexMethod*>
find_analysis_methods(const DexClass& cls, const std::string& name) {
  std::unordered_map<int, DexMethod*> analysis_method_map;
  for (const auto& dm : cls.get_dmethods()) {
    if (name == dm->get_name()->c_str()) {
      auto const v =
          std::next(dm->get_proto()->get_args()->get_type_list().begin(), 1);
      if (*v == DexType::make_type("[S")) {
        analysis_method_map.emplace(1, dm);
      } else {
        analysis_method_map.emplace(dm->get_proto()->get_args()->size(), dm);
      }
    }
  }
  return analysis_method_map;
}

std::unordered_map<int, DexMethod*> find_and_verify_analysis_method(
    const DexClass& cls, const std::string& method_name) {
  const auto& analysis_method_map = find_analysis_methods(cls, method_name);
  if (!analysis_method_map.empty()) {
    return analysis_method_map;
  }

  std::cerr << "[InstrumentPass] error: cannot find " << method_name << " in "
            << show(cls) << std::endl;
  for (auto&& m : cls.get_dmethods()) {
    std::cerr << " " << show(m) << std::endl;
  }
  exit(1);
}

// Find if the current insertion point lies within a try_start - try_end block.
// If it does, return its corresponding catch block.
MethodItemEntry* find_try_block(IRCode* code, IRList::iterator insert_point) {
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
                            IRList::iterator insert_point,
                            MethodItemEntry* catch_block) {
  if (catch_block) {
    auto try_start = new MethodItemEntry(TRY_START, catch_block);
    code->insert_before(insert_point, *try_start);
  }
}

void insert_try_end_instr(IRCode* code,
                          IRList::iterator insert_point,
                          MethodItemEntry* catch_block) {
  if (catch_block) {
    auto try_end = new MethodItemEntry(TRY_END, catch_block);
    code->insert_before(insert_point, *try_end);
  }
}

void insert_invoke_instr(IRCode* code,
                         IRList::iterator insert_point,
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

// When the number of bit vectors for a method is more than 5, they are added to
// an array and the array is passed to the analysis function onMethodExitBB()
// along with the method identifier.
// Algorithm:
// Bit vectors: v_0, v_1, v_2, v_3, v_4, v_5
// num_vectors: 6
// Initialize size of new array: OPCODE: CONST <v_array>, 6
// Initialize a new empty array: OPCODE: NEW_ARRAY <v_array>, [S
// For each bit vector <v_i>:
//     initialize index:     OPCODE: CONST <v_pos>, [position]
//     Add to array:         OPCODE: APUT_OBJECT <v_i>, <v_array> , <v_pos>
// Finally, call analysis_method:
//     initialize method id: OPCODE: CONST <v_mId>, [method_id]
//     actual call:          OPCODE: INVOKE_STATIC <v_mId>, <v_array>
//
// Example:
// OPCODE: CONST v59, 6
// OPCODE: NEW_ARRAY v59, [S
// OPCODE: IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v59
// OPCODE: CONST v60, 0
// OPCODE: APUT v52, v59, v60
// OPCODE: CONST v61, 1
// OPCODE: APUT v53, v59, v61
// OPCODE: CONST v62, 2
// OPCODE: APUT v54, v59, v62
// OPCODE: CONST v63, 3
// OPCODE: APUT v55, v59, v63
// OPCODE: CONST v64, 4
// OPCODE: APUT v56, v59, v64
// OPCODE: CONST v65, 5
// OPCODE: APUT v57, v59, v65
// OPCODE: CONST v66, 6171
// OPCODE: INVOKE_STATIC v66, v59,
//          Lcom/facebook/redex/dynamicanalysis/Analysis;.onMethodExitBB:(I[S)V
void insert_invoke_static_array_arg(IRCode* code,
                                    size_t method_id,
                                    DexMethod* method_onMethodExit,
                                    const std::vector<uint16_t>& reg_bb_vector,
                                    IRList::iterator insert_point) {
  // If num_vectors >5, create array of all bit vectors. Add as argument to
  // invoke call.
  size_t num_vectors = reg_bb_vector.size();
  // Create new array.
  IRInstruction* array_size_inst = new IRInstruction(OPCODE_CONST);
  array_size_inst->set_literal(num_vectors);
  const auto array_dest = code->allocate_temp();
  array_size_inst->set_dest(array_dest);

  IRInstruction* new_array_insn = new IRInstruction(OPCODE_NEW_ARRAY);
  new_array_insn->set_type(make_array_type(get_short_type()));
  new_array_insn->set_arg_word_count(1);
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
  std::vector<uint16_t> index_reg(num_vectors);

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
    aput_inst[i]->set_arg_word_count(3);
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
  invoke_inst->set_arg_word_count(2);
  invoke_inst->set_src(0, reg_method_id);
  invoke_inst->set_src(1, array_dest);

  insert_invoke_instr(code, insert_point, method_id_inst, invoke_inst);
} // namespace
void insert_invoke_static_call_bb(IRCode* code,
                                  size_t method_id,
                                  DexMethod* method_onMethodExit,
                                  const std::vector<uint16_t>& reg_bb_vector) {
  for (auto mie = code->begin(); mie != code->end(); ++mie) {
    if (mie->type == MFLOW_OPCODE &&
        (mie->insn->opcode() == OPCODE_RETURN ||
         mie->insn->opcode() == OPCODE_RETURN_OBJECT ||
         mie->insn->opcode() == OPCODE_RETURN_VOID)) {
      IRInstruction* method_id_inst = new IRInstruction(OPCODE_CONST);
      method_id_inst->set_literal(method_id);
      auto reg_method_id = code->allocate_temp();
      method_id_inst->set_dest(reg_method_id);

      assert(reg_bb_vector.size() != 0);
      if (reg_bb_vector.size() <= 5) {
        IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
        // Method to be invoked depends on the number of vectors in current
        // method. The '1' is added to count first argument i.e. method_id.
        invoke_inst->set_method(method_onMethodExit);
        invoke_inst->set_arg_word_count(reg_bb_vector.size() + 1);
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
      });
}

// num_vectors is the number of 16-bit vectors used to capture all the basic
// blocks in the current method.
// For every basic block, add an instruction to calculate
// bit_vector[n] OR (1<<block_id).

int instrument_onBasicBlockBegin(
    IRCode* code,
    DexMethod* method,
    const std::unordered_map<int, DexMethod*>& method_onMethodExit_map,
    size_t method_id,
    int& all_bbs,
    int& num_blocks_instrumented,
    int& all_methods_inst,
    std::unordered_map<int, std::string>& method_id_name_map,
    std::unordered_map<int, int>& id_numbb_map) {
  assert(code != nullptr);

  code->build_cfg(/* editable */ false);
  const auto& blocks = code->cfg().blocks();

  TRACE(INSTRUMENT, 7, "[%s] Number of Basic Blocks: %zu\n",
        SHOW(method->get_name()), blocks.size());

  // It is done this way to avoid using ceil() or double/float casting.
  const size_t num_vectors = (blocks.size() + 16 - 1) / 16;

  TRACE(INSTRUMENT, 7, "[%s] Number of Vectors: %zu\n",
        SHOW(method->get_name()), num_vectors);

  all_bbs += blocks.size();

  // Add <num_vectors> 16 bit integers to the beginning to method. These will be
  // used as basic block bit vectors.
  std::vector<uint16_t> reg_bb_vector(num_vectors);
  std::vector<IRInstruction*> const_inst_int(num_vectors);
  for (size_t reg_index = 0; reg_index < num_vectors; ++reg_index) {
    const_inst_int.at(reg_index) = new IRInstruction(OPCODE_CONST);
    const_inst_int.at(reg_index)->set_literal(0);
    reg_bb_vector.at(reg_index) = code->allocate_temp();
    const_inst_int.at(reg_index)->set_dest(reg_bb_vector.at(reg_index));
  }
  // Before the basic blocks are instrumented, at the method exit points,
  // we add an INVOKE_STATIC call to send the bit vector to logs.
  // This is done before actual instrumentation to get the updated CFG after
  // adding edges to this invoke call.
  // The INVOKE call takes (num_vectors+1) arguments: 1-Method ID,
  // num_vectors-Bit Vectors.
  size_t index_to_method = (num_vectors > 5) ? 1 : num_vectors + 1;
  assert(method_onMethodExit_map.count(index_to_method));
  insert_invoke_static_call_bb(code, method_id,
                               method_onMethodExit_map.at(index_to_method),
                               reg_bb_vector);

  for (cfg::Block* block : blocks) {
    const size_t block_vector_index = block->id() / 16;
    // Add instruction to calculate 'basic_block_bit_vector |= 1 << block->id()'
    // We use OPCODE_OR_INT_LIT16 to prevent inserting an extra CONST
    // instruction into the bytecode.
    IRInstruction* or_inst = new IRInstruction(OPCODE_OR_INT_LIT16);
    or_inst->set_literal(static_cast<int16_t>(1ULL << (block->id() % 16)));
    or_inst->set_src(0, reg_bb_vector.at(block_vector_index));
    or_inst->set_dest(reg_bb_vector.at(block_vector_index));

    // Find where to insert the newly created instruction block.
    auto insert_point = find_or_insn_insert_point(block);

    // We do not instrument a Basic block if:
    // 1. It only has internal or MOVE instructions.
    // 2. BB has no opcodes.
    if (insert_point == block->end() || block->num_opcodes() < 1) {
      TRACE(INSTRUMENT, 7, "No instrumentation to block: %s\n",
            SHOW(std::string(method->get_fully_deobfuscated_name()) +
                 std::to_string(block->id())));
      continue;
    }
    num_blocks_instrumented++;
    code->insert_before(insert_point, or_inst);
  }
  auto method_name = method->get_fully_deobfuscated_name();
  assert(!method_id_name_map.count(method_id));
  method_id_name_map.emplace(method_id, method_name);
  id_numbb_map.emplace(method_id, blocks.size());

  // Find a point at the beginning of the method to insert the init
  // instruction. This initializes bit vector registers to 0.
  auto insert_point_init = std::find_if_not(
      code->begin(), code->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });

  for (size_t reg_index = 0; reg_index < num_vectors; ++reg_index) {
    code->insert_before(insert_point_init, const_inst_int.at(reg_index));
  }

  TRACE(INSTRUMENT, 7, "Id: %zu Method: %s\n", method_id, SHOW(method_name));
  TRACE(INSTRUMENT, 7, "After Instrumentation Full:\n %s\n", SHOW(code));

  method_id += num_vectors;
  all_methods_inst++;
  return method_id;
}

void instrument_onMethodBegin(DexMethod* method,
                              int index,
                              DexMethod* method_onMethodBegin) {
  IRCode* code = method->get_code();
  assert(code != nullptr);

  IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
  const_inst->set_literal(index);
  const auto reg_dest = code->allocate_temp();
  const_inst->set_dest(reg_dest);

  IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_inst->set_method(method_onMethodBegin);
  invoke_inst->set_arg_word_count(1);
  invoke_inst->set_src(0, reg_dest);

  // TODO(minjang): Consider using get_param_instructions.
  // Try to find a right insertion point: the entry point of the method.
  // We skip any fall throughs and IOPCODE_LOAD_PARRM*.
  auto insert_point = std::find_if_not(
      code->begin(), code->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });

  if (insert_point == code->end()) {
    // No load params. So just insert before the head.
    insert_point = code->begin();
  } else if (insert_point->type == MFLOW_DEBUG) {
    // Right after the load params, there could be DBG_SET_PROLOGUE_END.
    // Skip if there is a following POSITION, too. For example:
    // 1: OPCODE: IOPCODE_LOAD_PARAM_OBJECT v1
    // 2: OPCODE: IOPCODE_LOAD_PARAM_OBJECT v2
    // 3: DEBUG: DBG_SET_PROLOGUE_END
    // 4: POSITION: foo.java:42 (this might be optional.)
    // <== Instrumentation code will be inserted here.
    //
    std::advance(insert_point,
                 std::next(insert_point)->type != MFLOW_POSITION ? 1 : 2);
  } else {
    // Otherwise, insert_point can be used directly.
  }

  code->insert_before(code->insert_before(insert_point, invoke_inst),
                      const_inst);

  if (debug) {
    for (auto it = code->begin(); it != code->end(); ++it) {
      if (it == insert_point) {
        TRACE(INSTRUMENT, 9, "<==== insertion\n");
        TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
        ++it;
        if (it != code->end()) {
          TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
          ++it;
          if (it != code->end()) {
            TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
          }
        }
        TRACE(INSTRUMENT, 9, "\n");
        break;
      }
      TRACE(INSTRUMENT, 9, "%s\n", SHOW(*it));
    }
  }
}

// Find a sequence of opcode that creates a static array. Patch the array size.
void patch_array_size(DexClass& analysis_cls,
                      const char* array_name,
                      const int array_size) {
  DexMethod* clinit = analysis_cls.get_clinit();
  always_assert(clinit != nullptr);

  auto* code = clinit->get_code();
  bool patched = false;
  walk::matching_opcodes_in_block(
      *clinit,
      // Don't find OPCODE_CONST. It might be deduped with others, or changing
      // this const can affect other instructions. (Well, we might have a
      // unique const number though.) So, just create a new const load
      // instruction. LocalDCE can clean up the redundant instructions.
      std::make_tuple(/* m::is_opcode(OPCODE_CONST), */
                      m::is_opcode(OPCODE_NEW_ARRAY),
                      m::is_opcode(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT),
                      m::is_opcode(OPCODE_SPUT_OBJECT)),
      [&](DexMethod* method,
          cfg::Block*,
          const std::vector<IRInstruction*>& insts) {
        assert(method == clinit);
        if (insts[2]->get_field()->get_name()->str() != array_name) {
          return;
        }

        IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
        const_inst->set_literal(array_size);
        const auto reg_dest = code->allocate_temp();
        const_inst->set_dest(reg_dest);
        insts[0]->set_src(0, reg_dest);
        for (auto& mie : InstructionIterable(code)) {
          if (mie.insn == insts[0]) {
            code->insert_before(code->iterator_to(mie), const_inst);
            patched = true;
            return;
          }
        }
      });

  if (!patched) {
    std::cerr << "[InstrumentPass] error: cannot patch array size."
              << std::endl;
    std::cerr << show(clinit->get_code()) << std::endl;
    exit(1);
  }

  TRACE(INSTRUMENT, 2, "%s array was patched: %d\n", array_name, array_size);
}

void patch_static_field(DexClass& analysis_cls,
                        const char* field_name,
                        const int new_number) {
  DexMethod* clinit = analysis_cls.get_clinit();
  always_assert(clinit != nullptr);

  // Find the sput with the given field name.
  auto* code = clinit->get_code();
  IRInstruction* sput_inst = nullptr;
  IRList::iterator insert_point;
  for (auto& mie : InstructionIterable(code)) {
    auto* insn = mie.insn;
    if (insn->opcode() == OPCODE_SPUT &&
        insn->get_field()->get_name()->str() == field_name) {
      sput_inst = insn;
      insert_point = code->iterator_to(mie);
      break;
    }
  }

  // SPUT can be null if the original field value was encoded in the
  // static_values_off array. And consider simplifying using make_concrete.
  if (sput_inst == nullptr) {
    TRACE(INSTRUMENT, 2, "sput %s was deleted; creating it\n", field_name);
    sput_inst = new IRInstruction(OPCODE_SPUT);
    sput_inst->set_field(
        DexField::make_field(DexType::make_type(analysis_cls.get_name()),
                             DexString::make_string(field_name),
                             DexType::make_type("I")));
    insert_point =
        code->insert_after(code->get_param_instructions().end(), sput_inst);
  }

  // Create a new const instruction just like patch_stat_array_size.
  IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
  const_inst->set_literal(new_number);
  const auto reg_dest = code->allocate_temp();
  const_inst->set_dest(reg_dest);

  sput_inst->set_src(0, reg_dest);
  code->insert_before(insert_point, const_inst);
  TRACE(INSTRUMENT, 2, "%s was patched: %d\n", field_name, new_number);
}

void write_method_index_file(const std::string& file_name,
                             const std::vector<DexMethod*>& id_vector) {
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  for (size_t i = 0; i < id_vector.size(); ++i) {
    ofs << i + 1 << ", " << id_vector[i]->get_fully_deobfuscated_name()
        << std::endl;
  }
  TRACE(INSTRUMENT, 2, "Index file was written to: %s\n", file_name.c_str());
}

void write_basic_block_index_file(
    const std::string& file_name,
    const std::unordered_map<int, std::string>& id_name_map,
    const std::unordered_map<int, int>& id_numbb_map) {
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  for (auto it = id_name_map.begin(); it != id_name_map.end(); ++it) {
    ofs << it->first << ", " << show(it->second) << ","
        << id_numbb_map.find(it->first)->second << std::endl;
  }
  TRACE(INSTRUMENT, 2, "Index file was written to: %s\n", file_name.c_str());
} // namespace

void do_simple_method_tracing(DexClass* analysis_cls,
                              DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& pm,
                              const InstrumentPass::Options& options) {
  const auto& method_onMethodBegin_map = find_and_verify_analysis_method(
      *analysis_cls, options.analysis_method_name);

  // This is set to 1 because current method level tracing invoke call has only
  // 1 argument.
  always_assert(method_onMethodBegin_map.count(1));
  auto method_onMethodBegin = method_onMethodBegin_map.at(1);
  // Instrument and build the method id map at the same time.
  std::unordered_map<DexMethod*, int /*id*/> method_id_map;
  std::vector<DexMethod*> method_id_vector;
  int index = 0;
  int excluded = 0;
  auto scope = build_class_scope(stores);
  walk::methods(scope, [&](DexMethod* method) {
    if (method->get_code() == nullptr) {
      return;
    }
    if (method == method_onMethodBegin ||
        method == analysis_cls->get_clinit()) {
      ++excluded;
      TRACE(INSTRUMENT, 2, "Excluding analysis method: %s\n", SHOW(method));
      return;
    }

    // Handle whitelist and blacklist.
    const auto& cls_name = show(method->get_class());
    const auto& method_name = method->get_name()->str();
    if (!options.whitelist.empty()) {
      if (is_included(method_name, cls_name, options.whitelist)) {
        TRACE(INSTRUMENT, 7, "Whitelist: included: %s\n", SHOW(method));
      } else {
        ++excluded;
        TRACE(INSTRUMENT, 8, "Whitelist: excluded: %s\n", SHOW(method));
        return;
      }
    }

    // In case of a conflict, when an entry is present in both blacklist
    // and whitelist, the blacklist is given priority and the entry
    // is not instrumented.
    if (is_included(method_name, cls_name, options.blacklist)) {
      ++excluded;
      TRACE(INSTRUMENT, 7, "Blacklist: excluded: %s\n", SHOW(method));
      return;
    }

    assert(!method_id_map.count(method));
    method_id_map.emplace(method, ++index);
    method_id_vector.push_back(method);
    TRACE(INSTRUMENT, 5, "%d: %s\n", method_id_map.at(method), SHOW(method));

    // NOTE: Only for testing D8607258! We test the method index file is
    // safely uploaded. So we enabled this pass but prevent actual
    // instrumentation.
    //
    // instrument_onMethodBegin(method, index * options.num_stats_per_method,
    //                          method_onMethodBegin);
  });

  TRACE(INSTRUMENT,
        1,
        "%d methods were instrumented (%d methods were excluded)\n",
        index,
        excluded);

  // Patch stat array size.
  patch_array_size(*analysis_cls, "sMethodStats",
                   index * options.num_stats_per_method);

  // Patch method count constant.
  patch_static_field(*analysis_cls, "sMethodCount", index);

  write_method_index_file(cfg.metafile(options.metadata_file_name),
                          method_id_vector);

  pm.incr_metric("Instrumented", index);
  pm.incr_metric("Excluded", excluded);
}

// Basic block instrumentation algorithm.
// 1. Initialize a variable b_vector to 0. Number of bits in b_vector >= Number
// of basic blocks in the method.
// 2. For every block with id b_id, set <b_id>th bit in the bit-vector.
// 3. Before RETURN, INVOKE onMethodExit(method_id, b_vector).
// Example:
// Original CFG
//     ++++++++++           ++++++++++          ++++++++++
//     + block0 +  ---->    + block1 +  ---->   + block2 +
//     +        +           +        +          + Return +
//     ++++++++++           ++++++++++          ++++++++++
// Initialize empty bit vectors
//     +++++++++++++++
//     + CONST v0, 0 +           ++++++++++          ++++++++++
//     + block0      +  ---->    + block1 +  ---->   + block2 +
//     +             +           +        +          + Return +
//     +++++++++++++++           ++++++++++          ++++++++++
// Instrument individual basic block to set <i>th bit in the bit vector, where
// i = 2^(block_id)
//     +++++++++++++++++++
//     + CONST v0, 0     +      ++++++++++++++++++         ++++++++++++++++++
//     + CONST v1, 1<<0  +      + CONST v1, 1<<1 +          + CONST v1, 1<<2 +
//     + OR v0, v1       +      + OR v0, v1      +          + OR v0, v1      +
//     + block0          +  --->  + block1       +  ---->   + block2         +
//     +++++++++++++++          ++++++++++++++++++          + Return         +
//                                                          ++++++++++++++++++
//
// Before return, add an INVOKE call to send the bit vector for analysis
// +++++++++++++++++++
// + CONST v0, 0     +       ++++++++++++++++++      +++++++++++++++++++++++++++
// + CONST v1, 1<<0  +       + CONST v1, 1<<1 +      + CONST v1, 1<<2          +
// + OR v0, v1       +       + OR v0, v1      +      + OR v0, v1               +
// + block0          +  -->  + block1         +  --> + block2                  +
// +++++++++++++++++++       ++++++++++++++++++      + CONST v2, method_id     +
//                                                   + INVOKE v2,v0, Analysis()+
//                                                   + Return                  +
//                                                   +++++++++++++++++++++++++++
//
// ============
// OPTIMIZATION:
// We use OR_INT_LIT16 to prevent adding CONST v1, 1<<0 instruction at each
// block.
//     +++++++++++++++++++
//     + CONST v0, 0     +       ++++++++++++++++++++     +++++++++++++++++++++
//     + OR_LIT16 v0, 1<<0 +     + OR_LIT16 v0, 1<<1+     + OR_LIT16 v0, 1<<2 +
//     + block0            + --> + block1           + --> + block2            +
//     +++++++++++++++           ++++++++++++++++++       + Return            +
//                                                        +++++++++++++++++++++
// Optimized final instrumented code
// +++++++++++++++++++
// + CONST v0, 0     +       ++++++++++++++++++      +++++++++++++++++++++++++++
// + OR_LIT16 v0, 1  +       + OR_LIT16 v0, 2 +      + OR_LIT16 v0, 4          +
// + block0          +  -->  + block1         +  --> + block2                  +
// +++++++++++++++++++       ++++++++++++++++++      + CONST v2, method_id     +
//                                                   + INVOKE v2,v0, Analysis()+
//                                                   + Return                  +
//                                                   +++++++++++++++++++++++++++

void do_basic_block_tracing(DexClass* analysis_cls,
                            DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& pm,
                            const InstrumentPass::Options& options) {

  const auto& method_onMethodExit_map = find_and_verify_analysis_method(
      *analysis_cls, options.analysis_method_name);

  size_t method_index = 1;
  int all_bb_nums = 0;
  int all_methods = 0;
  int all_bb_inst = 0;
  int all_method_inst = 0;
  std::unordered_map<int /*id*/, std::string> method_id_name_map;
  std::unordered_map<int /*id*/, int /*Num BBs*/> method_id_bb_map;
  auto scope = build_class_scope(stores);

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
  TRACE(INSTRUMENT, 7, "Number of classes: %d\n", cold_start_classes.size());

  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method == analysis_cls->get_clinit()) {
      return;
    }
    if (std::any_of(method_onMethodExit_map.begin(),
                    method_onMethodExit_map.end(),
                    [&](const auto& e) { return e.second == method; })) {
      return;
    }

    // Basic block tracing assumes whitelist or set of cold start classes.
    if ((!options.whitelist.empty() &&
         !is_included(method->get_name()->str(), method->get_class()->c_str(),
                      options.whitelist)) ||
        !is_included(method->get_name()->str(), method->get_class()->c_str(),
                     cold_start_classes)) {
      return;
    }

    // Blacklist has priority over whitelist or cold start list.
    if (is_included(method->get_name()->str(), method->get_class()->c_str(),
                    options.blacklist)) {
      TRACE(INSTRUMENT, 9, "Blacklist: excluded: %s\n", SHOW(method));
      return;
    }

    TRACE(INSTRUMENT, 9, "Whitelist: included: %s\n", SHOW(method));
    all_methods++;
    method_index = instrument_onBasicBlockBegin(
        &code, method, method_onMethodExit_map, method_index, all_bb_nums,
        all_bb_inst, all_method_inst, method_id_name_map, method_id_bb_map);
  });
  patch_array_size(*analysis_cls, "sBasicBlockStats", method_index);

  write_basic_block_index_file(cfg.metafile(options.metadata_file_name),
                               method_id_name_map, method_id_bb_map);
  TRACE(INSTRUMENT, 3,
        "Instrumented %d methods(%d blocks) out of %d (Number of Blocks: "
        "%d).\n",
        (all_method_inst - 1), all_bb_inst, all_methods, all_bb_nums);
}

} // namespace

void InstrumentPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& pm) {
  if (m_options.analysis_class_name.empty()) {
    std::cerr << "[InstrumentPass] error: empty analysis class name."
              << std::endl;
    exit(1);
  }

  // Get the analysis class.
  DexType* analysis_class_type = g_redex->get_type(
      DexString::get_string(m_options.analysis_class_name.c_str()));
  if (analysis_class_type == nullptr) {
    std::cerr << "[InstrumentPass] error: cannot find analysis class: "
              << m_options.analysis_class_name << std::endl;
    exit(1);
  }

  DexClass* analysis_cls = g_redex->type_class(analysis_class_type);
  always_assert(analysis_cls != nullptr);

  // Check whether the analysis class is in the primary dex. We use a heuristic
  // that looks the last 12 characters of the location of the given dex.
  auto dex_loc = analysis_cls->get_dex_location();
  if (dex_loc.size() < 12 /* strlen("/classes.dex") == 12 */ ||
      dex_loc.substr(dex_loc.size() - 12) != "/classes.dex") {
    std::cerr << "[InstrumentPass] Analysis class must be in the primary dex. "
                 "It was in "
              << dex_loc << std::endl;
    exit(1);
  }

  // Just do the very minimal common work here: load the analysis class.
  // Each instrumentation strategy worker function will do its own job.
  TRACE(INSTRUMENT,
        3,
        "Loaded analysis class: %s (%s)\n",
        m_options.analysis_class_name.c_str(),
        analysis_cls->get_dex_location().c_str());

  if (m_options.instrumentation_strategy == "simple_method_tracing") {
    do_simple_method_tracing(analysis_cls, stores, cfg, pm, m_options);
  } else if (m_options.instrumentation_strategy == "basic_block_tracing") {
    do_basic_block_tracing(analysis_cls, stores, cfg, pm, m_options);
  } else {
    std::cerr << "[InstrumentPass] Unknown instrumentation strategy.\n";
  }
}

static InstrumentPass s_pass;
