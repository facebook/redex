/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InjectDebug.h"

#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexContext.h"
#include "ToolsCommon.h"
#include "TypeInference.h"

InjectDebug::InjectDebug(const std::string& outdir,
                         const std::vector<std::string>& dex_files)
    : m_conf(Json::Value(), outdir), m_dex_files(dex_files) {
  if (!g_redex) {
    g_redex = new RedexContext();
  }
}

InjectDebug::~InjectDebug() {
  delete g_redex;
  g_redex = nullptr;
}

void InjectDebug::run() {
  load_dex();
  inject_all();
  write_dex();
}

void InjectDebug::load_dex() {
  DexStore root_store("classes");
  root_store.set_dex_magic(load_dex_magic_from_dex(m_dex_files[0].c_str()));
  m_stores.emplace_back(std::move(root_store));

  dex_stats_t input_totals;
  std::vector<dex_stats_t> input_dexes_stats;
  redex::load_classes_from_dexes_and_metadata(m_dex_files, m_stores,
                                              input_totals, input_dexes_stats);
}

void InjectDebug::inject_register(
    IRCode* ir_code,
    const IRList::iterator& ir_it,
    const type_inference::TypeEnvironment& type_env,
    reg_t reg) {
  // Get general type of register (either java object or primitive)
  DexType* reg_type;
  if (type_env.get_type(reg).element() == IRType::REFERENCE) {
    reg_type = type::java_lang_Object();
  } else {
    reg_type = type::_int();
  }

  DexString* reg_string = DexString::make_string("v" + std::to_string(reg));
  ir_code->insert_before(
      ir_it,
      std::make_unique<DexDebugOpcodeStartLocal>(reg, reg_string, reg_type));
}

void InjectDebug::inject_method(DexMethod* dex_method, int* line_start) {
  IRCode* ir_code = dex_method->get_code();
  if (ir_code == nullptr) return;

  DexDebugItem* dbg = ir_code->get_debug_item();
  if (dbg != nullptr) dbg->get_entries().clear();

  ir_code->build_cfg(false);
  type_inference::TypeInference type_inf(ir_code->cfg());
  type_inf.run(dex_method);

  std::unordered_map<const IRInstruction*, type_inference::TypeEnvironment>&
      type_envs = type_inf.get_type_environments();

  boost::sub_range<IRList> param_instructions =
      ir_code->get_param_instructions();
  auto ir_it = param_instructions.begin();

  // Emit local variables for every param
  for (; ir_it != param_instructions.end(); ++ir_it) {
    if (ir_it->type == MethodItemType::MFLOW_OPCODE) {
      type_inf.analyze_instruction(ir_it->insn, &type_envs.at(ir_it->insn));
      if (ir_it->insn->has_dest()) {
        inject_register(ir_code, ir_it, type_envs.at(ir_it->insn),
                        ir_it->insn->dest());
      }
    }
  }

  for (; ir_it != ir_code->end(); ++ir_it) {
    switch (ir_it->type) {
    case MethodItemType::MFLOW_OPCODE: {
      ir_code->insert_before(
          ir_it, std::make_unique<DexPosition>(dex_method->get_name(),
                                               dex_method->get_name(),
                                               *line_start));
      ++(*line_start);
      // Make debugger stop at every instruction, and provide local variables to
      // debug each instruction's source and destination registers
      type_inf.analyze_instruction(ir_it->insn, &type_envs.at(ir_it->insn));
      for (reg_t src_reg : ir_it->insn->srcs_vec()) {
        inject_register(ir_code, ir_it, type_envs.at(ir_it->insn), src_reg);
      }
      if (ir_it->insn->has_dest()) {
        inject_register(ir_code, ir_it, type_envs.at(ir_it->insn),
                        ir_it->insn->dest());
      }

      if (ir_it->insn->has_move_result_pseudo()) {
        // Must check dest() of next instruction for the result of current instr
        IRList::iterator next_it = std::next(ir_it);
        if (next_it->insn->has_dest()) {
          type_inf.analyze_instruction(next_it->insn,
                                       &type_envs.at(next_it->insn));
          inject_register(ir_code, ir_it, type_envs.at(next_it->insn),
                          next_it->insn->dest());
        }
        ++ir_it;
      }
      break;
    }
    // Remove any previous instances of debug entries
    case MethodItemType::MFLOW_DEBUG:
    case MethodItemType::MFLOW_POSITION: {
      ir_it->type = MethodItemType::MFLOW_FALLTHROUGH;
      break;
    }
    default:
      break;
    };
  }
}

void InjectDebug::inject_all() {
  // Use IR instructions to generate dex debug information
  for (DexStore& store : m_stores) {
    for (DexClasses& classes : store.get_dexen()) {
      for (DexClass* dex_class : classes) {
        // Line numbers within a single class should be unique so that JDB
        // can find a unique location with class name and line number
        int line_start = 0;
        for (DexMethod* dex_method : dex_class->get_dmethods()) {
          inject_method(dex_method, &line_start);
        }
        for (DexMethod* dex_method : dex_class->get_vmethods()) {
          inject_method(dex_method, &line_start);
        }
      }
    }
  }
}

void InjectDebug::write_dex() {
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
  instruction_lowering::run(m_stores, true);

  for (size_t store_num = 0; store_num < m_stores.size(); ++store_num) {
    DexStore& store = m_stores[store_num];
    for (size_t i = 0; i < store.get_dexen().size(); ++i) {
      std::string filename =
          redex::get_dex_output_name(m_conf.get_outdir(), store, i);
      DexOutput dout = DexOutput(filename.c_str(), // filename
                                 &store.get_dexen()[i], // classes
                                 nullptr, // locator_index
                                 false, // normal_primary_dex
                                 store_num,
                                 i, // dex_number,
                                 DebugInfoKind::BytecodeDebugger,
                                 nullptr, // iodi_metadata
                                 m_conf, // redex options config
                                 pos_mapper.get(), // position_mapper
                                 nullptr, // method_to_id
                                 nullptr, // code_debug_lines
                                 nullptr // post_lowering
      );
      dout.prepare(SortMode::DEFAULT, {SortMode::DEFAULT}, m_conf,
                   m_stores[0].get_dex_magic());
      dout.write();
    }
  }
}
