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

DexMethod* find_analysis_method(const DexClass& cls, const std::string& name) {
  auto dmethods = cls.get_dmethods();
  auto it =
      std::find_if(dmethods.begin(), dmethods.end(), [&name](DexMethod* m) {
        return name == m->get_name()->str();
      });
  return it == dmethods.end() ? nullptr : *it;
}


int instrument_onBasicBlockBegin(
    DexMethod* method,
    DexMethod* method_onMethodExit,
    int method_id,
    int& all_bbs,
    int& num_blocks_instrumented,
    std::unordered_map<std::string, int>& method_id_map,
    std::vector<std::string>& method_vector) {
  IRCode* code = method->get_code();
  if (code == nullptr) {
    return method_id;
  }

  code->build_cfg(/* editable */ false);
  const auto& blocks = code->cfg().blocks();

  TRACE(INSTRUMENT, 7, "[%s] Number of Basic Blocks: %zu\n",
        SHOW(method->get_name()), blocks.size());

  // For now, only instrumenting methods with less than 16 blocks.
  // There are cases for redex generated methods where the deobfuscated name is
  // empty. We map the debofucated name to index into the method id, thus we
  // skip the cases where this name is empty.
  if (blocks.size() > 16 || method->get_deobfuscated_name().empty()) {
    return method_id;
  }
  all_bbs += blocks.size();

  // Add a 16 bit integer to the beginning to method. This will be used as
  // the basic block bit vector.
  IRInstruction* const_inst_int = new IRInstruction(OPCODE_CONST);
  const_inst_int->set_literal(0);
  auto reg_bb_vector = code->allocate_temp();
  const_inst_int->set_dest(reg_bb_vector);

  for (cfg::Block* block : blocks) {

    // Add instruction to calculate
    // [basic_block_bit_vector |= 1 << block->id()]
    // We use OPCODE_OR_INT_LIT16 to prevent inserting an extra CONST
    // instruction into the bytecode.
    IRInstruction* or_inst = new IRInstruction(OPCODE_OR_INT_LIT16);
    or_inst->set_literal(static_cast<int16_t>(1ULL << block->id()));
    or_inst->set_src(0, reg_bb_vector);
    or_inst->set_dest(reg_bb_vector);

    // Find where to insert the newly created instruction block.
    // After every invoke instruction, the value returned from the function is
    // moved to a register. The instruction used to move depends on the type of
    // return value. Our instrumentation should skip over all these move_result
    // instructions.
    auto insert_point = std::find_if_not(
        block->begin(), block->end(), [&](const MethodItemEntry& mie) {
          auto mie_op = mie.insn->opcode();
          return (mie.type == MFLOW_OPCODE &&
                  (opcode::is_internal(mie_op) ||
                   mie_op == OPCODE_MOVE ||
                   mie_op == OPCODE_MOVE_WIDE ||
                   mie_op == OPCODE_MOVE_EXCEPTION ||
                   mie_op == OPCODE_MOVE_OBJECT ||
                   mie_op == OPCODE_MOVE_RESULT ||
                   mie_op == OPCODE_MOVE_RESULT_OBJECT ||
                   mie_op == OPCODE_MOVE_RESULT_WIDE)) ||
                 (mie.type == MFLOW_TRY && (mie.tentry->type == TRY_END ||
                                            mie.tentry->type == TRY_START));
        });

    // We do not instrument a Basic block if:
    // 1. It only has internal or MOVE instructions.
    // 2. BB has no opcodes.
    if (insert_point == block->end() || block->num_opcodes() < 1) {
      TRACE(INSTRUMENT, 7, "No instrumentation to block: %s(%d)\n",
            SHOW(std::string(method->get_deobfuscated_name()) +
                 std::to_string(block->id())),
            block->num_opcodes());
      continue;
    }
    num_blocks_instrumented++;
    code->insert_before(insert_point, or_inst);
  }
  auto method_name = method->get_deobfuscated_name();
  assert(!method_id_map.count(method_name));
  method_id_map.emplace(method_name, method_id);
  method_vector.push_back(method_name);

  // Find a point at the beginning of the method to insert the init instruction.
  // This initializes bit vector to 0.
  auto insert_point_init = std::find_if_not(
      code->begin(), code->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });

  code->insert_before(insert_point_init, const_inst_int);

  // Once all the basic blocks are instrumented, before exiting from the method,
  // we add an INVOKE_STATIC call to send the bit vector to logs.
  // This call takes 2 arguments: 1-Method ID, 2-Bit Vector.
  for (auto mie = code->begin(); mie != code->end(); ++mie) {
    if (mie->type == MFLOW_OPCODE &&
        (mie->insn->opcode() == OPCODE_RETURN ||
         mie->insn->opcode() == OPCODE_RETURN_OBJECT ||
         mie->insn->opcode() == OPCODE_RETURN_VOID ||
         mie->insn->opcode() == OPCODE_GOTO)) {
      IRInstruction* method_id_inst = new IRInstruction(OPCODE_CONST);
      method_id_inst->set_literal(method_id);
      auto reg_method_id = code->allocate_temp();
      method_id_inst->set_dest(reg_method_id);

      IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
      invoke_inst->set_method(method_onMethodExit);
      invoke_inst->set_arg_word_count(2);
      invoke_inst->set_src(0, reg_method_id);
      invoke_inst->set_src(1, reg_bb_vector);

      // Finding if the current insertion point lies within a try_catchblock.
      // TODO : This can be done more efficiently. Update this.
      auto mie_try = code->begin();
      auto try_start_block = mie_try;
      bool try_start_found = false;
      MethodItemEntry* catch_block;
      while (mie_try != mie) {
        if (mie_try->type == MFLOW_TRY && mie_try->tentry->type == TRY_START) {
          try_start_found = true;
          try_start_block = mie_try;
          catch_block = mie_try->tentry->catch_start;
        }
        mie_try++;
      }
      if (try_start_found) {
        for (mie_try = try_start_block; mie_try != mie; ++mie_try) {
          if (mie_try->type == MFLOW_TRY && mie_try->tentry->type == TRY_END) {
            try_start_found = false;
          }
        }
      }

      // When inserting an INVOKE instruction within a try_catch block, in order
      // to prevent adding an extra throw edge to the CFG, we split the block
      // into two. We insert a TRY_END instruction before our instrumentation
      // INVOKE call and a TRY_START after.
      if (try_start_found) {
        auto try_start = new MethodItemEntry(TRY_START, catch_block);
        auto try_end = new MethodItemEntry(TRY_END, catch_block);

        code->insert_before(
            code->insert_before(
                code->insert_before(code->insert_before(mie, *try_start),
                                    invoke_inst),
                method_id_inst),
            *try_end);
      } else {
        code->insert_before(code->insert_before(mie, invoke_inst),
                            method_id_inst);
      }
    }
  }
  TRACE(INSTRUMENT, 7, "Id: %zu Method: %s\n", method_id++, SHOW(method_name));

  TRACE(INSTRUMENT, 7, "After Instrumentation Full:\n %s\n", SHOW(code));
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
      // this const can affect other instructions. (Well, we might have a unique
      // const number though.) So, just create a new const load instruction.
      // LocalDCE can clean up the redundant instructions.
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

template <typename T>
void write_index_file(const std::string& file_name,
                      const std::vector<T>& id_vector) {
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);
  for (size_t i = 0; i < id_vector.size(); ++i) {
    ofs << i + 1 << ", " << show(id_vector[i]) << std::endl;
  }
  TRACE(INSTRUMENT, 2, "Index file was written to: %s\n", file_name.c_str());
}

DexMethod* find_and_verify_analysis_method(const DexClass& cls,
                                           const std::string& method_name) {
  DexMethod* analysis_method = find_analysis_method(cls, method_name);
  if (analysis_method != nullptr) {
    return analysis_method;
  }

  std::cerr << "[InstrumentPass] error: cannot find " << method_name << " in "
            << show(cls) << std::endl;
  for (auto&& m : cls.get_dmethods()) {
    std::cerr << " " << show(m) << std::endl;
  }
  exit(1);
}

void do_simple_method_tracing(DexClass* analysis_cls,
                              DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& pm,
                              const InstrumentPass::Options& options) {
  DexMethod* method_onMethodBegin = find_and_verify_analysis_method(
      *analysis_cls, options.analysis_method_name);

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

  write_index_file<DexMethod*>(cfg.metafile(options.metadata_file_name),
                               method_id_vector);

  pm.incr_metric("Instrumented", index);
  pm.incr_metric("Excluded", excluded);
}

void do_basic_block_tracing(DexClass* analysis_cls,
                            DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& pm,
                            const InstrumentPass::Options& options) {
  DexMethod* method_onMethodExit = find_and_verify_analysis_method(
      *analysis_cls, options.analysis_method_name);

  // For each indivdual basic block from every method, assign them an
  // identifier and add a jump to onBasicBlockBegin() at the beginning.
  // onBasicBlockBegin() will set the touch variable when a basic block is
  // accessed at runtime.
  int method_index = 1;
  int all_bb_nums = 0;
  int num_methods = 0;
  int all_bb_inst = 0;
  // Map Basic block identifier to its name(MethodName + BlockID).
  std::unordered_map<std::string, int /*id*/> method_id_map;
  // This vector is used to get references to the blocks.
  std::vector<std::string> method_vector;
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

  walk::methods(scope, [&](DexMethod* method) {
    if (method == method_onMethodExit || method == analysis_cls->get_clinit()) {
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
    num_methods++;
    method_index = instrument_onBasicBlockBegin(
        method, method_onMethodExit, method_index, all_bb_nums, all_bb_inst,
        method_id_map, method_vector);
  });
  patch_array_size(*analysis_cls, "sBasicBlockStats", method_index);

  write_index_file<std::string>(cfg.metafile(options.metadata_file_name),
                                method_vector);
  TRACE(
      INSTRUMENT, 3,
      "Instrumented %d methods(%d blocks) out of %d (Number of Blocks: %d).\n",
      (method_index - 1), all_bb_inst, num_methods, all_bb_nums);
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
