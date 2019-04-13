/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Instrument.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "InterDexPassPlugin.h"
#include "Match.h"
#include "Show.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// sMethodStats is sharded to sMethodStats1 to sMethodStatsN.
static constexpr size_t NUM_SHARDS = 8;

class InstrumentInterDexPlugin : public interdex::InterDexPassPlugin {
 public:
  InstrumentInterDexPlugin(size_t max_analysis_methods)
      : m_max_analysis_methods(max_analysis_methods) {}

  void configure(const Scope& scope, ConfigFiles& cfg) override{};

  bool should_skip_class(const DexClass* clazz) override { return false; }

  void gather_refs(const interdex::DexInfo& dex_info,
                   const DexClass* cls,
                   std::vector<DexMethodRef*>& mrefs,
                   std::vector<DexFieldRef*>& frefs,
                   std::vector<DexType*>& trefs,
                   std::vector<DexClass*>* erased_classes,
                   bool should_not_relocate_methods_of_class) override {}

  size_t reserve_mrefs() override {
    // In each dex, we will introduce more method refs from analysis methods.
    // This makes sure that the inter-dex pass keeps space for new method refs.
    return m_max_analysis_methods;
  }

  DexClasses additional_classes(const DexClassesVector& outdex,
                                const DexClasses& classes) override {
    return {};
  }

  void cleanup(const std::vector<DexClass*>& scope) override {}

 private:
  const size_t m_max_analysis_methods;
};

// For example, say that "Lcom/facebook/debug/" is in the set. We match either
// "^Lcom/facebook/debug/*" or "^Lcom/facebook/debug;".
bool match_class_name(std::string cls_name,
                      const std::unordered_set<std::string>& set) {
  always_assert(cls_name.back() == ';');
  // We also support exact class name (e.g., "Lcom/facebook/Debug;")
  if (set.count(cls_name)) {
    return true;
  }
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
  for (const auto& m : cls.get_dmethods()) {
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

// TODO(minjang): We could simply utilize already existing analysis methods
// instead of making an array. For an instance, to handle 42 bit vecgors, We
// could call 8 times of onMethodExitBB(SSSSS) and 1 time of onMethodExitBB(SS).
//
// ---
// When the number of bit vectors for a method is more than 5, they are added to
// an array and the array is passed to the analysis function onMethodExitBB()
// along with the method identifier.
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
}

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
int instrument_onBasicBlockBegin(
    IRCode* code,
    DexMethod* method,
    const std::unordered_map<int, DexMethod*>& method_onMethodExit_map,
    size_t method_id,
    int& all_bbs,
    int& num_blocks_instrumented,
    int& all_methods_inst,
    std::map<int, std::pair<std::string, int>>& method_id_name_map,
    std::map<size_t, int>& bb_vector_stat) {
  assert(code != nullptr);

  code->build_cfg(/* editable */ false);
  const auto& blocks = code->cfg().blocks();

  // It is done this way to avoid using ceil() or double/float casting.
  // Since we reserve the MSB as the end marker, we divide by 15.
  const size_t num_vectors = (blocks.size() + 15 - 1) / 15;

  TRACE(INSTRUMENT, 7, "[%s] Basic Blocks: %zu, Necessary vectors: %zu\n",
        SHOW(method->get_name()), blocks.size(), num_vectors);

  all_bbs += blocks.size();

  // Add <num_vectors> shorts to the beginning to method. These will be used as
  // basic block 16-bit vectors.
  std::vector<uint16_t> reg_bb_vector(num_vectors);
  std::vector<IRInstruction*> const_inst_int(num_vectors);
  for (size_t reg_index = 0; reg_index < num_vectors; ++reg_index) {
    const_inst_int.at(reg_index) = new IRInstruction(OPCODE_CONST);
    // The very last bit is for the end marker: the unset bit. Otherwise, set
    // the MSB to indicate continuation.
    const_inst_int.at(reg_index)->set_literal(
        reg_index == num_vectors - 1 ? 0 : -32768);
    reg_bb_vector.at(reg_index) = code->allocate_temp();
    const_inst_int.at(reg_index)->set_dest(reg_bb_vector.at(reg_index));
  }

  // Before the basic blocks are instrumented, at the method exit points,
  // we add an INVOKE_STATIC call to send the bit vector to logs. This is done
  // before actual instrumentation to get the updated CFG after adding edges to
  // this invoke call. The INVOKE call takes (num_vectors + 1) arguments:
  // Method ID (actually, the short array offset) and bit vectors * n.
  size_t index_to_method = (num_vectors > 5) ? 1 : num_vectors + 1;
  ++bb_vector_stat[num_vectors];
  assert(method_onMethodExit_map.count(index_to_method));
  insert_invoke_static_call_bb(code, method_id,
                               method_onMethodExit_map.at(index_to_method),
                               reg_bb_vector);

  for (cfg::Block* block : blocks) {
    const size_t block_vector_index = block->id() / 15;
    // Add instruction to calculate 'basic_block_bit_vector |= 1 << block->id()'
    // We use OPCODE_OR_INT_LIT16 to prevent inserting an extra CONST
    // instruction into the bytecode.
    IRInstruction* or_inst = new IRInstruction(OPCODE_OR_INT_LIT16);
    or_inst->set_literal(static_cast<int16_t>(1ULL << (block->id() % 15)));
    or_inst->set_src(0, reg_bb_vector.at(block_vector_index));
    or_inst->set_dest(reg_bb_vector.at(block_vector_index));

    // Find where to insert the newly created instruction block.
    auto insert_point = find_or_insn_insert_point(block);

    // We do not instrument a Basic block if:
    // 1. It only has internal or MOVE instructions.
    // 2. BB has no opcodes.
    if (insert_point == block->end() || block->num_opcodes() < 1) {
      TRACE(INSTRUMENT, 7, "No instrumentation to block: %s\n",
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
                      const std::string array_name,
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

  TRACE(INSTRUMENT, 2, "%s array was patched: %d\n", SHOW(array_name),
        array_size);
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

void write_basic_block_index_file(
    const std::string& file_name,
    const std::map<int, std::pair<std::string, int>>& id_name_map) {
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  for (const auto& p : id_name_map) {
    ofs << p.first << "," << p.second.first << "," << p.second.second
        << std::endl;
  }
  TRACE(INSTRUMENT, 2, "Index file was written to: %s\n", file_name.c_str());
} // namespace

auto find_sharded_analysis_methods(const DexClass& cls,
                                   const std::string& method_name) -> auto {
  std::unordered_map<std::string, int> names;
  for (size_t i = 1; i <= NUM_SHARDS; ++i) {
    names[method_name + std::to_string(i)] = i;
  }

  std::unordered_map<int, DexMethod*> methods;
  for (const auto& m : cls.get_dmethods()) {
    auto found = names.find(m->get_name()->str());
    if (found != names.end()) {
      methods[found->second] = m;
    }
  }

  if (methods.size() != NUM_SHARDS) {
    std::cerr << "[InstrumentPass] error: failed to find all " << method_name
              << "[1-" << NUM_SHARDS << "] in " << show(cls) << std::endl;
    for (const auto& m : cls.get_dmethods()) {
      std::cerr << " " << show(m) << std::endl;
    }
    exit(1);
  }

  return std::make_pair(methods, names);
}

void do_simple_method_tracing(DexClass* analysis_cls,
                              DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& pm,
                              const InstrumentPass::Options& options) {
  const auto& analysis_methods = find_sharded_analysis_methods(
      *analysis_cls, options.analysis_method_name);
  const auto& analysis_method_map = analysis_methods.first;
  const auto& analysis_method_names = analysis_methods.second;

  // Write metadata file with more information.
  const auto& file_name = conf.metafile(options.metadata_file_name);
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);

  // Write meta info of the meta file: the type of the meta file and version.
  ofs << "#,simple-method-tracing,1.0" << std::endl;

  size_t method_id = 0;
  int excluded = 0;
  std::unordered_set<std::string> method_names;
  std::vector<DexMethod*> to_instrument;

  auto worker = [&](DexMethod* method, size_t& total_size) -> int {
    const auto& name = method->get_deobfuscated_name();
    always_assert_log(
        !method_names.count(name),
        "Deobfuscated method names must be unique, but found duplicate: %s",
        SHOW(name));
    method_names.insert(name);

    if (method->get_code() == nullptr) {
      ofs << "M,-1," << name << ",0,\"" << vshow(method->get_access(), true)
          << "\"\n";
      return 0;
    }

    const size_t sum_opcode_sizes =
        method->get_code()->sum_non_internal_opcode_sizes();
    total_size += sum_opcode_sizes;

    // Excluding analysis methods myselves.
    if (analysis_method_names.count(method->get_name()->str()) ||
        method == analysis_cls->get_clinit()) {
      ++excluded;
      TRACE(INSTRUMENT, 2, "Excluding analysis method: %s\n", SHOW(method));
      ofs << "M,-1," << name << "," << sum_opcode_sizes << ",\""
          << "MYSELF " << vshow(method->get_access(), true) << "\"\n";
      return 0;
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
        return 0;
      }
    }

    // In case of a conflict, when an entry is present in both blacklist
    // and whitelist, the blacklist is given priority and the entry
    // is not instrumented.
    if (is_included(method_name, cls_name, options.blacklist)) {
      ++excluded;
      TRACE(INSTRUMENT, 7, "Blacklist: excluded: %s\n", SHOW(method));
      ofs << "M,-1," << name << "," << sum_opcode_sizes << ",\""
          << "BLACKLIST " << vshow(method->get_access(), true) << "\"\n";
      return 0;
    }

    TRACE(INSTRUMENT, 5, "%zu: %s\n", method_id, SHOW(method));
    assert(to_instrument.size() == method_id);
    to_instrument.push_back(method);

    // Emit metadata to the file.
    ofs << "M," << method_id << "," << name << "," << sum_opcode_sizes << ",\""
        << vshow(method->get_access(), true /*is_method*/) << "\"\n";
    ++method_id;
    return 1;
  };

  auto scope = build_class_scope(stores);
  TypeSystem ts(scope);

  // We now have sharded method stats arrays. Say we instrument 11 methods and
  // have three arrays. Each array will have ceil(11/3) = 3 methods, and the
  // last array may have smaller number of methods.
  //                    array1        array2       array3
  //     method ids  [0, 1, 2, 3]  [4, 5, 6, 7]  [8, 9, 10]
  //   array indices [0, 1, 2, 3]  [0, 1, 2, 3]  [0, 1,  2]
  // In order to do that, we need to know the total number of methods to be
  // instrumented. We don't know this number until iterating all methods while
  // processing exclusions. We take a two-pass approach:
  //  1) For all methods, collect (method id, method) pairs and write meta data.
  //  2) Do actual instrumentation.
  for (const auto& cls : scope) {
    const auto& cls_name = cls->get_deobfuscated_name();
    always_assert_log(
        !method_names.count(cls_name),
        "Deobfuscated class names must be unique, but found duplicate: %s",
        SHOW(cls_name));
    method_names.insert(cls_name);

    int instrumented = 0;
    size_t total_size = 0;
    for (auto dmethod : cls->get_dmethods()) {
      instrumented += worker(dmethod, total_size);
    }
    for (auto vmethod : cls->get_vmethods()) {
      instrumented += worker(vmethod, total_size);
    }

    ofs << "C," << cls_name << "," << total_size << ","
        << (instrumented == 0 ? "NONE" : std::to_string(instrumented)) << ","
        << cls->get_dmethods().size() << "," << cls->get_vmethods().size()
        << ",\"" << vshow(cls->get_access(), false /*is_method*/) << "\"\n";

    // Enumerate all super and interface classes for this class.
    const auto& obj_type = DexType::get_type("Ljava/lang/Object;");
    std::stringstream ss_parents;
    for (const auto& e : ts.parent_chain(cls->get_type())) {
      // Exclude myself and obvious java.lang.Object.
      if (e != obj_type && e != cls->get_type()) {
        ss_parents << show_deobfuscated(e) << " ";
      }
    }
    if (ss_parents.tellp() > 0) {
      ofs << "P," << cls_name << ",\"" << ss_parents.str() << "\"\n";
    }

    std::stringstream ss_interfaces;
    for (const auto& e : ts.get_all_super_interfaces(cls->get_type())) {
      ss_interfaces << show_deobfuscated(e) << " ";
    }
    if (ss_interfaces.tellp() > 0) {
      ofs << "I," << cls_name << ",\"" << ss_interfaces.str() << "\"\n";
    }
  }

  // Now we know the total number of methods to be instrumented. Do some
  // computations and actual instrumentation.
  const size_t kTotalSize = to_instrument.size();
  const size_t kShardSize =
      static_cast<size_t>(std::ceil(double(kTotalSize) / NUM_SHARDS));
  TRACE(INSTRUMENT, 4, "%zu methods to be instrumented; shard size: %zu\n",
        kTotalSize, kShardSize);
  for (size_t i = 0; i < kTotalSize; ++i) {
    TRACE(INSTRUMENT, 7, "Sharded %zu => [%zu][%zu] %s\n", i, i / kShardSize,
          i % kShardSize, SHOW(to_instrument[i]));
    instrument_onMethodBegin(to_instrument[i],
                             (i % kShardSize) * options.num_stats_per_method,
                             analysis_method_map.at((i / kShardSize) + 1));
  }

  TRACE(INSTRUMENT,
        1,
        "%d methods were instrumented (%d methods were excluded)\n",
        method_id,
        excluded);

  // Patch stat array size.
  for (size_t i = 0; i < NUM_SHARDS; ++i) {
    patch_array_size(
        *analysis_cls, "sMethodStats" + std::to_string(i + 1),
        (i == NUM_SHARDS - 1
             ? options.num_stats_per_method * (kTotalSize - kShardSize * i)
             : options.num_stats_per_method * kShardSize));
  }

  // Patch method count constant.
  always_assert(method_id == kTotalSize);
  patch_static_field(*analysis_cls, "sMethodCount", kTotalSize);

  ofs.close();
  TRACE(INSTRUMENT, 2, "Index file was written to: %s\n", file_name.c_str());

  pm.incr_metric("Instrumented", method_id);
  pm.incr_metric("Excluded", excluded);
}

// A simple bit-vector basic block instrumentation algorithm
//
//  Example) Original CFG
//   +--------+       +--------+       +--------+
//   | block0 | ----> | block1 | ----> | block2 |
//   |        |       |        |       | Return |
//   +--------+       +--------+       +--------+
//
//  Instrumented CFG:
//   - Initialize bit vector(s) at the beginning
//   - Set <bb_id>-th bit in the vector using or-lit/16. So, the bit vector is a
//     short type. We don't use a 32-bit int; no such or-lit/32 instruction.
//   - Before RETURN, insert INVOKE onMethodExit(method_id, bit_vectors).
//
//   +------------------+     +------------------+     +-----------------------+
//   | * CONST v0, 0    | --> | * OR_LIT16 v0, 2 | --> | * OR_LIT16 v0, 4      |
//   | * OR_LIT16 v0, 1 |     |   block1         |     |   block2              |
//   |   block0         |     |                  |     | * CONST v2, method_id |
//   +------------------+     +------------------+     | * INVOKE v2,v0, ...   |
//                                                     |   Return              |
//                                                     +-----------------------+
//
void do_basic_block_tracing(DexClass* analysis_cls,
                            DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& pm,
                            const InstrumentPass::Options& options) {

  const auto& method_onMethodExit_map = find_and_verify_analysis_method(
      *analysis_cls, options.analysis_method_name);

  size_t method_index = 1;
  int all_bb_nums = 0;
  int all_methods = 0;
  int all_bb_inst = 0;
  int all_method_inst = 0;
  std::map<int /*id*/, std::pair<std::string, int /*number of BBs*/>>
      method_id_name_map;
  auto scope = build_class_scope(stores);

  auto interdex_list = conf.get_coldstart_classes();
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

  std::map<size_t /* num_vectors */, int /* count */> bb_vector_stat;
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
        (options.only_cold_start_class &&
         !is_included(method->get_name()->str(), method->get_class()->c_str(),
                      cold_start_classes))) {
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
        all_bb_inst, all_method_inst, method_id_name_map, bb_vector_stat);
  });
  patch_array_size(*analysis_cls, "sBasicBlockStats", method_index);

  write_basic_block_index_file(conf.metafile(options.metadata_file_name),
                               method_id_name_map);

  double cumulative = 0.;
  TRACE(INSTRUMENT, 4, "BB vector stats:\n");
  for (const auto& p : bb_vector_stat) {
    double percent = (double)p.second * 100. / (float)all_method_inst;
    cumulative += percent;
    TRACE(INSTRUMENT, 4, " %3zu bit vectors: %6d (%6.3lf%%, %6.3lf%%)\n",
          p.first, p.second, percent, cumulative);
  }

  TRACE(INSTRUMENT, 3,
        "Instrumented %d methods and %d blocks, out of %d methods and %d "
        "blocks\n",
        (all_method_inst - 1), all_bb_inst, all_methods, all_bb_nums);
}

std::unordered_set<std::string> load_blacklist_file(
    const std::string& file_name) {
  // Assume the file simply enumerates blacklisted names.
  std::unordered_set<std::string> ret;
  std::ifstream ifs(file_name);
  assert_log(ifs, "Can't open blacklist file: %s\n", file_name.c_str());

  std::string line;
  while (ifs >> line) {
    ret.insert(line);
  }

  TRACE(INSTRUMENT, 3, "Loaded %zu blacklist entries from %s\n", ret.size(),
        file_name.c_str());
  return ret;
}
} // namespace

void InstrumentPass::configure_pass(const JsonWrapper& jw) {
  jw.get("instrumentation_strategy", "", m_options.instrumentation_strategy);
  jw.get("analysis_class_name", "", m_options.analysis_class_name);
  jw.get("analysis_method_name", "", m_options.analysis_method_name);
  std::vector<std::string> list;
  jw.get("blacklist", {}, list);
  for (const auto& e : list) {
    m_options.blacklist.insert(e);
  }
  jw.get("whitelist", {}, list);
  for (const auto& e : list) {
    m_options.whitelist.insert(e);
  }
  jw.get("blacklist_file_name", "", m_options.blacklist_file_name);
  jw.get("metadata_file_name", "instrument-mapping.txt",
         m_options.metadata_file_name);
  jw.get("num_stats_per_method", 1, m_options.num_stats_per_method);
  jw.get("only_cold_start_class", true, m_options.only_cold_start_class);

  // Make a small room for additional method refs during InterDex.
  interdex::InterDexRegistry* registry =
      static_cast<interdex::InterDexRegistry*>(
          PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
  registry->register_plugin("INSTRUMENT_PASS_PLUGIN", []() {
    return new InstrumentInterDexPlugin(NUM_SHARDS);
  });
}

void InstrumentPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& pm) {
  if (!pm.get_redex_options().instrument_pass_enabled) {
    TRACE(INSTRUMENT, 1, "--enable-instrument-pass is not specified.\n");
    return;
  }

  // Append black listed classes from the file, if exists.
  if (!m_options.blacklist_file_name.empty()) {
    for (const auto& e : load_blacklist_file(m_options.blacklist_file_name)) {
      m_options.blacklist.insert(e);
    }
  }

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
  auto dex_loc = analysis_cls->get_location();
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
        analysis_cls->get_location().c_str());

  if (m_options.instrumentation_strategy == "simple_method_tracing") {
    do_simple_method_tracing(analysis_cls, stores, conf, pm, m_options);
  } else if (m_options.instrumentation_strategy == "basic_block_tracing") {
    do_basic_block_tracing(analysis_cls, stores, conf, pm, m_options);
  } else {
    std::cerr << "[InstrumentPass] Unknown instrumentation strategy.\n";
  }
}

static InstrumentPass s_pass;
