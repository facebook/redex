/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRCode.h"

#include <algorithm>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <memory>
#include <unordered_set>
#include <limits>
#include <list>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Transform.h"
#include "Util.h"

namespace {

int bytecount(int32_t v) {
  int bytecount = 4;
  if ((int32_t)((int8_t)(v & 0xff)) == v) {
    bytecount = 1;
  } else if ((int32_t)((int16_t)(v & 0xffff)) == v) {
    bytecount = 2;
  }
  return bytecount;
}

DexOpcode goto_for_offset(int32_t offset) {
  if (offset == 0) {
    return DOPCODE_GOTO_32;
  }
  switch (bytecount(offset)) {
  case 1:
    return DOPCODE_GOTO;
  case 2:
    return DOPCODE_GOTO_16;
  case 4:
    return DOPCODE_GOTO_32;
  default:
    always_assert_log(false, "Invalid bytecount %d", offset);
  }
}

namespace {

using namespace boost::bimaps;

struct Entry {};
struct Addr {};

typedef bimap<tagged<MethodItemEntry*, Entry>,
              unordered_set_of<tagged<uint32_t, Addr>>>
    EntryAddrBiMap;

} // namespace

static MethodItemEntry* get_target(
    const MethodItemEntry* mei,
    const EntryAddrBiMap& bm) {
  uint32_t base = bm.by<Entry>().at(const_cast<MethodItemEntry*>(mei));
  int offset = mei->dex_insn->offset();
  uint32_t target = base + offset;
  always_assert_log(
      bm.by<Addr>().count(target) != 0,
      "Invalid opcode target %08x[%p](%08x) %08x in get_target %s\n",
      base,
      mei,
      offset,
      target,
      SHOW(mei->insn));
  return bm.by<Addr>().at(target);
}

static void insert_branch_target(IRList* ir,
                                 MethodItemEntry* target,
                                 MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget(src);
  ir->insert_before(ir->iterator_to(*target), *(new MethodItemEntry(bt)));
}

// Returns true if the offset could be encoded without modifying ir.
bool encode_offset(IRList* ir,
                   MethodItemEntry* target_mie,
                   int32_t offset) {
  auto branch_op_mie = target_mie->target->src;
  auto insn = branch_op_mie->dex_insn;
  // A branch to the very next instruction does nothing. Replace with
  // fallthrough. The offset is measured in 16 bit code units, not
  // MethodItemEntries
  if (offset == static_cast<int32_t>(insn->size())) {
    branch_op_mie->type = MFLOW_FALLTHROUGH;
    delete target_mie->target;
    target_mie->type = MFLOW_FALLTHROUGH;
    return false;
  } else if (offset == 0) {
    auto nop = new DexInstruction(DOPCODE_NOP);
    ir->insert_before(ir->iterator_to(*branch_op_mie), nop);
    offset = -static_cast<int32_t>(nop->size());
    return false;
  }

  auto bop = branch_op_mie->dex_insn->opcode();
  if (dex_opcode::is_goto(bop)) {
    auto goto_op = goto_for_offset(offset);
    if (goto_op != bop) {
      branch_op_mie->dex_insn = new DexInstruction(goto_op);
      delete insn;
      return false;
    }
  } else if (dex_opcode::is_conditional_branch(bop)) {
    // if-* opcodes can only encode up to 16-bit offsets. To handle larger ones
    // we use a goto/32 and have the inverted if-* opcode skip over it. E.g.
    //
    //   if-gt <large offset>
    //   nop
    //
    // becomes
    //
    //   if-le <label>
    //   goto/32 <large offset>
    //   label:
    //   nop
    if (bytecount(offset) > 2) {
      branch_op_mie->dex_insn = new DexInstruction(DOPCODE_GOTO_32);

      auto inverted = dex_opcode::invert_conditional_branch(bop);
      MethodItemEntry* mei = new MethodItemEntry(new DexInstruction(inverted));
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        mei->dex_insn->set_src(i, insn->src(i));
      }
      ir->insert_before(ir->iterator_to(*branch_op_mie), *mei);

      // this iterator should always be valid -- an if-* instruction cannot
      // be the last opcode in a well-formed method
      auto next_insn_it = std::next(ir->iterator_to(*branch_op_mie));
      insert_branch_target(ir, &*next_insn_it, mei);

      delete insn;
      return false;
    }
  } else {
    always_assert_log(bop == DOPCODE_FILL_ARRAY_DATA,
                      "Unexpected opcode %s",
                      SHOW(*branch_op_mie));
  }
  always_assert(offset != 0);
  branch_op_mie->dex_insn->set_offset(offset);
  return true;
}

static bool multi_target_compare_case_key(const BranchTarget* a,
                                          const BranchTarget* b) {
  return (a->case_key < b->case_key);
}

static bool multi_contains_gaps(const std::vector<BranchTarget*>& targets) {
  int32_t key = targets.front()->case_key;
  for (auto target : targets) {
    if (target->case_key != key) return true;
    key++;
  }
  return false;
}

static void insert_multi_branch_target(IRList* ir,
                                       int32_t case_key,
                                       MethodItemEntry* target,
                                       MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget(src, case_key);
  ir->insert_before(ir->iterator_to(*target), *(new MethodItemEntry(bt)));
}

static int32_t read_int32(const uint16_t*& data) {
  int32_t result;
  memcpy(&result, data, sizeof(int32_t));
  data += 2;
  return result;
}

static void shard_multi_target(IRList* ir,
                               DexOpcodeData* fopcode,
                               MethodItemEntry* src,
                               const EntryAddrBiMap& bm) {
  const uint16_t* data = fopcode->data();
  uint16_t entries = *data++;
  auto ftype = fopcode->opcode();
  uint32_t base = bm.by<Entry>().at(src);
  if (ftype == FOPCODE_PACKED_SWITCH) {
    int32_t case_key = read_int32(data);
    for (int i = 0; i < entries; i++) {
      uint32_t targetaddr = base + read_int32(data);
      auto target = bm.by<Addr>().at(targetaddr);
      insert_multi_branch_target(ir, case_key, target, src);
      case_key++;
    }
  } else if (ftype == FOPCODE_SPARSE_SWITCH) {
    const uint16_t* tdata = data + 2 * entries;  // entries are 32b
    for (int i = 0; i < entries; i++) {
      int32_t case_key = read_int32(data);
      uint32_t targetaddr = base + read_int32(tdata);
      auto target = bm.by<Addr>().at(targetaddr);
      insert_multi_branch_target(ir, case_key, target, src);
    }
  } else {
    always_assert_log(false, "Bad fopcode 0x%04x in shard_multi_target", ftype);
  }
}

static void generate_branch_targets(
    IRList* ir,
    const EntryAddrBiMap& bm,
    const std::unordered_map<MethodItemEntry*, DexOpcodeData*>& entry_to_data) {
  for (auto miter = ir->begin(); miter != ir->end(); ++miter) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_DEX_OPCODE) {
      auto insn = mentry->dex_insn;
      if (dex_opcode::is_branch(insn->opcode())) {
        if (dex_opcode::is_switch(insn->opcode())) {
          auto* fopcode_entry = get_target(mentry, bm);
          auto* fopcode = entry_to_data.at(fopcode_entry);
          shard_multi_target(ir, fopcode, mentry, bm);
          delete fopcode;
          // TODO: erase fopcode from map
        } else {
          auto target = get_target(mentry, bm);
          insert_branch_target(ir, target, mentry);
        }
      }
    }
  }
}

static void associate_debug_entries(IRList* ir,
                                    DexDebugItem& dbg,
                                    const EntryAddrBiMap& bm) {
  for (auto& entry : dbg.get_entries()) {
    auto insert_point_it = bm.by<Addr>().find(entry.addr);
    if (insert_point_it == bm.by<Addr>().end()) {
      // This should not happen if our input is an "ordinary" dx/d8-generated
      // dex file, but things like IODI can generate debug entries that don't
      // correspond to code addresses.
      continue;
    }
    MethodItemEntry* mentry;
    switch (entry.type) {
    case DexDebugEntryType::Instruction:
      mentry = new MethodItemEntry(std::move(entry.insn));
      break;
    case DexDebugEntryType::Position:
      mentry = new MethodItemEntry(std::move(entry.pos));
      break;
    }
    ir->insert_before(ir->iterator_to(*insert_point_it->second), *mentry);
  }
  dbg.get_entries().clear();
}

// Insert MFLOW_TRYs and MFLOW_CATCHes
static void associate_try_items(IRList* ir,
                                DexCode& code,
                                const EntryAddrBiMap& bm) {
  // We insert the catches after the try markers to handle the case where the
  // try block ends on the same instruction as the beginning of the catch block.
  // We need to end the try block before we start the catch block, not vice
  // versa.
  //
  // The pairs have location first, then new catch entry second.
  std::vector<std::pair<MethodItemEntry*, MethodItemEntry*>> catches_to_insert;

  const auto& tries = code.get_tries();
  for (const auto& tri : tries) {
    MethodItemEntry* catch_start = nullptr;
    CatchEntry* last_catch = nullptr;
    for (const auto& catz : tri->m_catches) {
      auto catzop = bm.by<Addr>().at(catz.second);
      TRACE(MTRANS, 3, "try_catch %08x mei %p\n", catz.second, catzop);
      auto catch_mie = new MethodItemEntry(catz.first);
      catch_start = catch_start == nullptr ? catch_mie : catch_start;
      if (last_catch != nullptr) {
        last_catch->next = catch_mie;
      }
      last_catch = catch_mie->centry;
      // Delay addition of catch entries until after try entries
      catches_to_insert.emplace_back(catzop, catch_mie);
    }

    auto begin = bm.by<Addr>().at(tri->m_start_addr);
    TRACE(MTRANS, 3, "try_start %08x mei %p\n", tri->m_start_addr, begin);
    auto try_start = new MethodItemEntry(TRY_START, catch_start);
    ir->insert_before(ir->iterator_to(*begin), *try_start);
    uint32_t lastaddr = tri->m_start_addr + tri->m_insn_count;
    auto end = bm.by<Addr>().at(lastaddr);
    TRACE(MTRANS, 3, "try_end %08x mei %p\n", lastaddr, end);
    auto try_end = new MethodItemEntry(TRY_END, catch_start);
    ir->insert_before(ir->iterator_to(*end), *try_end);
  }

  for (const auto& pair : catches_to_insert) {
    ir->insert_before(ir->iterator_to(*pair.first), *pair.second);
  }
}

/*
 * Populate IRCode with load-param opcodes corresponding to the method
 * prototype. For example, a static method with proto "V(IJLfoo;)" and
 * no temp_regs will translate to
 *
 *   IOPCODE_LOAD_PARAM v0
 *   IOPCODE_LOAD_PARAM_WIDE v1
 *   IOPCODE_LOAD_PARAM_OBJECT v3
 */
void generate_load_params(const DexMethod* method,
                          size_t temp_regs,
                          IRCode* code) {
  auto args = method->get_proto()->get_args()->get_type_list();
  auto param_reg = temp_regs;
  if (!is_static(method)) {
    auto insn = new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
    insn->set_dest(param_reg++);
    code->push_back(insn);
  }
  for (DexType* arg : args) {
    IROpcode op;
    auto prev_reg = param_reg;
    if (is_wide_type(arg)) {
      param_reg += 2;
      op = IOPCODE_LOAD_PARAM_WIDE;
    } else {
      param_reg += 1;
      op = is_primitive(arg) ? IOPCODE_LOAD_PARAM : IOPCODE_LOAD_PARAM_OBJECT;
    }
    auto insn = new IRInstruction(op);
    insn->set_dest(prev_reg);
    code->push_back(insn);
  }
  code->set_registers_size(param_reg);
}

void translate_dex_to_ir(
    IRList* ir_list,
    const EntryAddrBiMap& bm,
    const std::unordered_map<MethodItemEntry*, DexOpcodeData*>& entry_to_data) {
  for (auto it = ir_list->begin(); it != ir_list->end(); ++it) {
    if (it->type != MFLOW_DEX_OPCODE) {
      continue;
    }
    auto* dex_insn = it->dex_insn;
    auto dex_op = dex_insn->opcode();
    auto op = opcode::from_dex_opcode(dex_op);
    auto* insn = new IRInstruction(op);

    IRInstruction* move_result_pseudo{nullptr};
    if (insn->dests_size()) {
      insn->set_dest(dex_insn->dest());
    } else if (opcode::may_throw(op)) {
      if (op == OPCODE_CHECK_CAST) {
        move_result_pseudo =
            new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        move_result_pseudo->set_dest(dex_insn->src(0));
      } else if (dex_insn->dests_size()) {
        IROpcode move_op;
        if (opcode_impl::dest_is_wide(op)) {
          move_op = IOPCODE_MOVE_RESULT_PSEUDO_WIDE;
        } else if (opcode_impl::dest_is_object(op)) {
          move_op = IOPCODE_MOVE_RESULT_PSEUDO_OBJECT;
        } else {
          move_op = IOPCODE_MOVE_RESULT_PSEUDO;
        }
        move_result_pseudo = new IRInstruction(move_op);
        move_result_pseudo->set_dest(dex_insn->dest());
      }
    }

    insn->set_arg_word_count(dex_insn->srcs_size()); // XXX: should we have a better API?
    for (size_t i = 0; i < dex_insn->srcs_size(); ++i) {
      insn->set_src(i, dex_insn->src(i));
    }
    if (dex_opcode::has_range(dex_op)) {
      insn->set_arg_word_count(dex_insn->range_size());
      for (size_t i = 0; i < dex_insn->range_size(); ++i) {
        insn->set_src(i, dex_insn->range_base() + i);
      }
    }
    if (dex_insn->has_string()) {
      insn->set_string(
          static_cast<const DexOpcodeString*>(dex_insn)->get_string());
    } else if (dex_insn->has_type()) {
      insn->set_type(static_cast<const DexOpcodeType*>(dex_insn)->get_type());
    } else if (dex_insn->has_field()) {
      insn->set_field(static_cast<const DexOpcodeField*>(dex_insn)->get_field());
    } else if (dex_insn->has_method()) {
      insn->set_method(
          static_cast<const DexOpcodeMethod*>(dex_insn)->get_method());
    } else if (dex_opcode::has_literal(dex_op)) {
      insn->set_literal(dex_insn->get_literal());
    } else if (op == OPCODE_FILL_ARRAY_DATA) {
      insn->set_data(entry_to_data.at(get_target(&*it, bm)));
    }

    insn->normalize_registers();

    it->type = MFLOW_OPCODE;
    it->insn = insn;
    if (move_result_pseudo != nullptr) {
      it = ir_list->insert_before(std::next(it),
                           *(new MethodItemEntry(move_result_pseudo)));
    }
  }
}

void balloon(DexMethod* method, IRList* ir_list) {
  auto dex_code = method->get_dex_code();
  auto instructions = dex_code->release_instructions();
  // This is a 1-to-1 map between MethodItemEntries of type MFLOW_OPCODE and
  // address offsets.
  EntryAddrBiMap bm;
  std::unordered_map<MethodItemEntry*, DexOpcodeData*> entry_to_data;

  uint32_t addr = 0;
  for (auto insn : *instructions) {
    MethodItemEntry* mei;
    if (insn->opcode() == DOPCODE_NOP ||
        dex_opcode::is_fopcode(insn->opcode())) {
      // We have to insert dummy entries for these opcodes so that try items
      // and debug entries that are adjacent to them can find the right
      // address.
      mei = new MethodItemEntry();
      if (dex_opcode::is_fopcode(insn->opcode())) {
        entry_to_data.emplace(mei, static_cast<DexOpcodeData*>(insn));
      }
    } else {
      mei = new MethodItemEntry(insn);
    }
    ir_list->push_back(*mei);
    bm.insert(EntryAddrBiMap::relation(mei, addr));
    TRACE(MTRANS, 5, "%08x: %s[mei %p]\n", addr, SHOW(insn), mei);
    addr += insn->size();
  }
  bm.insert(EntryAddrBiMap::relation(&*ir_list->end(), addr));

  generate_branch_targets(ir_list, bm, entry_to_data);
  associate_try_items(ir_list, *dex_code, bm);
  translate_dex_to_ir(ir_list, bm, entry_to_data);
  auto debugitem = dex_code->get_debug_item();
  if (debugitem) {
    associate_debug_entries(ir_list, *debugitem, bm);
  }
}

/**
 * Map the `DexPositions` to a newly created clone. At the same time, it
 * preserves the relationship between a position and it's parent.
 */
std::unordered_map<DexPosition*, std::unique_ptr<DexPosition>>
get_old_to_new_position_copies(IRList* ir_list) {
  std::unordered_map<DexPosition*, std::unique_ptr<DexPosition>>
      old_position_to_new;
  for (auto& mie : *ir_list) {
    if (mie.type == MFLOW_POSITION) {
      old_position_to_new.emplace(mie.pos.get(),
                                  std::make_unique<DexPosition>(*mie.pos));
    }
  }

  for (auto& old_to_new : old_position_to_new) {
    DexPosition* old_pos = old_to_new.first;
    auto new_pos = old_to_new.second.get();

    // There may be dangling pointers to parent positions that have been deleted
    // So, we can't use the [] operator here because it would add
    // nullptr as a value in the map which would cause a segfault on a later
    // iteration of the loop. `.at()` is also not an option for the same reason.
    new_pos->parent = nullptr;
    if (old_pos->parent != nullptr) {
      auto search = old_position_to_new.find(old_pos->parent);
      if (search != old_position_to_new.end()) {
        new_pos->parent = search->second.get();
      }
    }
  }

  return old_position_to_new;
}

// TODO: merge this and MethodSplicer.
IRList* deep_copy_ir_list(IRList* old_ir_list) {
  IRList* ir_list = new IRList();

  std::unordered_map<DexPosition*, std::unique_ptr<DexPosition>>
      old_position_to_new = get_old_to_new_position_copies(old_ir_list);

  // Create a clone for each of the entries
  // and a mapping from old pointers to new pointers.
  std::unordered_map<MethodItemEntry*, MethodItemEntry*> old_mentry_to_new;
  for (auto& mie : *old_ir_list) {
    MethodItemEntry* copy_mie = new MethodItemEntry();
    copy_mie->type = mie.type;
    old_mentry_to_new[&mie] = copy_mie;
  }

  // now fill the fields of the `copy_mie`s
  for (auto& mie : *old_ir_list) {

    auto copy_mie = old_mentry_to_new.at(&mie);
    switch (mie.type) {
    case MFLOW_TRY:
      copy_mie->tentry =
          new TryEntry(mie.tentry->type,
                       mie.tentry->catch_start
                           ? old_mentry_to_new[mie.tentry->catch_start]
                           : nullptr);
      break;
    case MFLOW_CATCH:
      copy_mie->centry = new CatchEntry(mie.centry->catch_type);
      copy_mie->centry->next =
          mie.centry->next ? old_mentry_to_new[mie.centry->next] : nullptr;
      break;
    case MFLOW_TARGET:
      copy_mie->target = new BranchTarget();
      copy_mie->target->type = mie.target->type;
      copy_mie->target->case_key = mie.target->case_key;
      copy_mie->target->src = old_mentry_to_new[mie.target->src];
      break;
    case MFLOW_OPCODE:
      copy_mie->insn = new IRInstruction(*mie.insn);
      break;
    case MFLOW_DEBUG:
      new (&copy_mie->dbgop)
          std::unique_ptr<DexDebugInstruction>(mie.dbgop->clone());
      break;
    case MFLOW_POSITION:
      copy_mie->pos = std::move(old_position_to_new[mie.pos.get()]);
      break;
    case MFLOW_FALLTHROUGH:
      break;
    case MFLOW_DEX_OPCODE:
      always_assert_log(false, "DexInstruction not expected here!");
    default:
      not_reached();
    }

    ir_list->push_back(*copy_mie);
  }

  return ir_list;
}

} // namespace

IRCode::IRCode(): m_ir_list(new IRList()) {}

IRCode::~IRCode() {
  m_ir_list->clear_and_dispose();
  delete m_ir_list;
}

IRCode::IRCode(DexMethod* method): m_ir_list(new IRList()) {
  auto* dc = method->get_dex_code();
  generate_load_params(
      method, dc->get_registers_size() - dc->get_ins_size(), this);
  balloon(const_cast<DexMethod*>(method), m_ir_list);
  m_dbg = dc->release_debug_item();
}

IRCode::IRCode(DexMethod* method, size_t temp_regs)
    : m_ir_list(new IRList()) {
  always_assert(method->get_dex_code() == nullptr);
  generate_load_params(method, temp_regs, this);
}

IRCode::IRCode(const IRCode& code) {
  IRList* old_ir_list = code.m_ir_list;
  m_ir_list = deep_copy_ir_list(old_ir_list);
  m_registers_size = code.m_registers_size;
  if (code.m_dbg) {
    m_dbg = std::make_unique<DexDebugItem>(*code.m_dbg);
  }
}

void IRCode::build_cfg(bool editable) {
  clear_cfg();
  m_cfg = std::make_unique<cfg::ControlFlowGraph>(
      m_ir_list, m_registers_size, editable);
}

void IRCode::clear_cfg() {
  if (!m_cfg) {
    return;
  }

  if (m_cfg->editable()) {
    m_registers_size = m_cfg->get_registers_size();
    m_ir_list = m_cfg->linearize();
  }

  m_cfg.reset();
  for (auto it = m_ir_list->begin(); it != m_ir_list->end();) {
    if (it->type == MFLOW_FALLTHROUGH) {
      it = m_ir_list->erase_and_dispose(it);
    } else {
      ++it;
    }
  }
}

bool IRCode::editable_cfg_built() const {
  return m_cfg != nullptr && m_cfg->editable();
}

namespace {

using RegMap = transform::RegMap;

const char* DEBUG_ONLY show_reg_map(RegMap& map) {
  for (auto pair : map) {
    TRACE(INL, 5, "%u -> %u\n", pair.first, pair.second);
  }
  return "";
}

uint16_t calc_outs_size(const IRCode* code) {
  uint16_t size {0};
  for (auto& mie : *code) {
    if (mie.type != MFLOW_DEX_OPCODE) {
      continue;
    }
    auto insn = mie.dex_insn;
    if (dex_opcode::is_invoke_range(insn->opcode())) {
      size = std::max(size, boost::numeric_cast<uint16_t>(insn->range_size()));
    } else if (dex_opcode::is_invoke(insn->opcode())) {
      size = std::max(size, boost::numeric_cast<uint16_t>(insn->srcs_size()));
    }
  }
  return size;
}

void calculate_ins_size(const DexMethod* method, DexCode* dex_code) {
  auto& args_list = method->get_proto()->get_args()->get_type_list();
  uint16_t ins_size{0};
  if (!is_static(method)) {
    ++ins_size;
  }
  for (auto arg : args_list) {
    if (is_wide_type(arg)) {
      ins_size += 2;
    } else {
      ++ins_size;
    }
  }
  dex_code->set_ins_size(ins_size);
}

/*
 * Gather the debug opcodes and DexPositions in :ir_list and put them in
 * :entries. As part of this process, we do some pruning of redundant
 * DexPositions. There are two scenarios where we want to eliminate them:
 *
 * 1) A DexPosition needs to be emitted iff it immediately precedes a dex
 * opcode. If there are multple DexPositions immediately before a given opcode,
 * only the last one needs to get emitted. Concretely:
 *
 *   .pos "LFoo;.a()V" Foo.java 123
 *   .pos "LFoo;.a()V" Foo.java 124 # this can be deleted
 *   const v0 0
 *
 * 2) If we have two identical consecutive DexPositions, only the first one
 * needs to be emitted. Concretely:
 *
 *   .pos "LFoo;.a()V" Foo.java 123
 *   const v0 0
 *   .pos "LFoo;.a()V" Foo.java 123 # this can be deleted
 *   const v0 0
 *
 * Note that this pruning is not done as part of StripDebugInfoPass as that
 * pass needs to run before inlining. Pruning needs to be done after all
 * optimizations have run, because the optimizations can create redundant
 * DexPositions.
 */
void gather_debug_entries(
    IRList* ir_list,
    const std::unordered_map<MethodItemEntry*, uint32_t>& entry_to_addr,
    std::vector<DexDebugEntry>* entries) {
  bool next_pos_is_root{false};
  // A root is the first DexPosition that precedes an opcode
  std::unordered_set<DexPosition*> roots;
  // The last root that we encountered on our reverse walk of the IRList
  DexPosition* last_root_pos{nullptr};
  for (auto it = ir_list->rbegin(); it != ir_list->rend(); ++it) {
    auto& mie = *it;
    if (mie.type == MFLOW_DEX_OPCODE) {
      next_pos_is_root = true;
    } else if (mie.type == MFLOW_POSITION && next_pos_is_root) {
      next_pos_is_root = false;
      // Check for consecutive duplicates
      if (last_root_pos != nullptr && *last_root_pos == *mie.pos) {
        roots.erase(last_root_pos);
      }
      last_root_pos = mie.pos.get();
      roots.emplace(last_root_pos);
    }
  }
  // DexPositions have parent pointers that refer to other DexPositions in the
  // same method body; we want to recursively preserve the referents as well.
  // The rest of the DexPositions can be eliminated.
  std::unordered_set<DexPosition*> positions_to_keep;
  for (DexPosition* pos : roots) {
    positions_to_keep.emplace(pos);
    DexPosition* parent{pos->parent};
    while (parent != nullptr && positions_to_keep.count(parent) == 0) {
      positions_to_keep.emplace(parent);
      parent = parent->parent;
    }
  }
  for (auto& mie : *ir_list) {
    if (mie.type == MFLOW_DEBUG) {
      entries->emplace_back(entry_to_addr.at(&mie), std::move(mie.dbgop));
    } else if (mie.type == MFLOW_POSITION &&
               positions_to_keep.count(mie.pos.get()) != 0) {
      entries->emplace_back(entry_to_addr.at(&mie), std::move(mie.pos));
    }
  }
}

} // namespace

// We can't output regions with more than 2^16 code units.
// But the IR has no such restrictions. This function splits up a large try
// region into many small try regions that have the exact same catch
// information.
//
// Also, try region boundaries must lie on instruction boundaries.
void IRCode::split_and_insert_try_regions(
    uint32_t start,
    uint32_t end,
    const DexCatches& catches,
    std::vector<std::unique_ptr<DexTryItem>>* tries) {

  const auto& get_last_addr_before = [this](uint32_t requested_addr) {
    uint32_t valid_addr = 0;
    for (const auto& mie : *m_ir_list) {
      if (mie.type == MFLOW_DEX_OPCODE) {
        auto insn_size = mie.dex_insn->size();
        if (valid_addr == requested_addr ||
            valid_addr + insn_size > requested_addr) {
          return valid_addr;
        }
        valid_addr += insn_size;
      }
    }
    always_assert_log(false, "no valid address for %d", requested_addr);
  };

  constexpr uint32_t max = std::numeric_limits<uint16_t>::max();
  while (start < end) {
    auto size = (end - start <= max)
                    ? end - start
                    : get_last_addr_before(start + max) - start;
    auto tri = std::make_unique<DexTryItem>(start, size);
    tri->m_catches = catches;
    tries->push_back(std::move(tri));
    start += size;
  }
}

std::unique_ptr<DexCode> IRCode::sync(const DexMethod* method) {
  auto dex_code = std::make_unique<DexCode>();
  try {
    calculate_ins_size(method, &*dex_code);
    dex_code->set_registers_size(m_registers_size);
    dex_code->set_outs_size(calc_outs_size(this));
    dex_code->set_debug_item(std::move(m_dbg));
    while (try_sync(dex_code.get()) == false)
      ;
  } catch (std::exception&) {
    fprintf(stderr, "Failed to sync %s\n%s\n", SHOW(method), SHOW(this));
    throw;
  }
  return dex_code;
}

bool IRCode::try_sync(DexCode* code) {
  std::unordered_map<MethodItemEntry*, uint32_t> entry_to_addr;
  uint32_t addr = 0;
  // Step 1, regenerate opcode list for the method, and
  // and calculate the opcode entries address offsets.
  TRACE(MTRANS, 5, "Emitting opcodes\n");
  for (auto miter = m_ir_list->begin(); miter != m_ir_list->end(); ++miter) {
    MethodItemEntry* mentry = &*miter;
    TRACE(MTRANS, 5, "Analyzing mentry %p\n", mentry);
    entry_to_addr[mentry] = addr;
    if (mentry->type == MFLOW_DEX_OPCODE) {
      TRACE(MTRANS, 5, "Emitting mentry %p at %08x\n", mentry, addr);
      addr += mentry->dex_insn->size();
    }
  }
  // Step 2, Branch relaxation: calculate branch offsets for if-* and goto
  // opcodes, resizing them where necessary. Since resizing opcodes affects
  // address offsets, we need to iterate this to a fixed point.
  //
  // For instructions that use address offsets but never need resizing (i.e.
  // switch and fill-array-data opcodes), we calculate their offsets after
  // we have reached the fixed point.
  TRACE(MTRANS, 5, "Recalculating branches\n");
  std::vector<MethodItemEntry*> multi_branches;
  std::unordered_map<MethodItemEntry*, std::vector<BranchTarget*>> multis;
  std::unordered_map<BranchTarget*, uint32_t> multi_targets;
  bool needs_resync = false;
  for (auto miter = m_ir_list->begin(); miter != m_ir_list->end(); ++miter) {
    MethodItemEntry* mentry = &*miter;
    if (entry_to_addr.find(mentry) == entry_to_addr.end()) {
      continue;
    }
    if (mentry->type == MFLOW_DEX_OPCODE) {
      auto opcode = mentry->dex_insn->opcode();
      if (dex_opcode::is_switch(opcode)) {
        multi_branches.push_back(mentry);
      }
    }
    if (mentry->type == MFLOW_TARGET) {
      BranchTarget* bt = mentry->target;
      if (bt->type == BRANCH_MULTI) {
        multis[bt->src].push_back(bt);
        multi_targets[bt] = entry_to_addr.at(mentry);
        // We can't fix the primary switch opcodes address until we emit
        // the fopcode, which comes later.
      } else if (bt->type == BRANCH_SIMPLE &&
                 dex_opcode::is_branch(bt->src->dex_insn->opcode())) {
        MethodItemEntry* branch_op_mie = bt->src;
        auto branch_addr = entry_to_addr.find(branch_op_mie);
        always_assert_log(branch_addr != entry_to_addr.end(),
                          "%s refers to nonexistent branch instruction",
                          SHOW(*mentry));
        int32_t branch_offset = entry_to_addr.at(mentry) - branch_addr->second;
        needs_resync |= !encode_offset(m_ir_list, mentry, branch_offset);
      }
    }
  }
  if (needs_resync) {
    return false;
  }

  size_t num_align_nops{0};
  auto& opout = code->reset_instructions();
  for (auto& mie : *m_ir_list) {
    // We are assuming that fill-array-data-payload opcodes are always at
    // the end of the opcode stream (we enforce that during instruction
    // lowering). I.e. they are only followed by other fill-array-data-payload
    // opcodes. So adjusting their addresses here does not require re-running
    // branch relaxation.
    entry_to_addr.at(&mie) += num_align_nops;
    if (mie.type == MFLOW_TARGET &&
        mie.target->src->dex_insn->opcode() == DOPCODE_FILL_ARRAY_DATA) {
      // This MFLOW_TARGET is right before a fill-array-data-payload opcode,
      // so we should make sure its address is aligned
      if (entry_to_addr.at(&mie) & 1) {
        opout.push_back(new DexInstruction(DOPCODE_NOP));
        ++entry_to_addr.at(&mie);
        ++num_align_nops;
      }
      mie.target->src->dex_insn->set_offset(entry_to_addr.at(&mie) -
                                            entry_to_addr.at(mie.target->src));
      continue;
    }
    if (mie.type != MFLOW_DEX_OPCODE) {
      continue;
    }
    TRACE(MTRANS, 6, "Emitting insn %s\n", SHOW(mie.dex_insn));
    opout.push_back(mie.dex_insn);
  }
  addr += num_align_nops;

  TRACE(MTRANS, 5, "Emitting multi-branches\n");
  // Step 3, generate multi-branch fopcodes
  for (auto multiopcode : multi_branches) {
    auto& targets = multis[multiopcode];
    auto multi_insn = multiopcode->dex_insn;
    std::sort(targets.begin(), targets.end(), multi_target_compare_case_key);
    always_assert_log(
        !targets.empty(), "need to have targets for %s", SHOW(*multiopcode));
    if (multi_contains_gaps(targets)) {
      // Emit sparse.
      const size_t count = (targets.size() * 4) + 2;
      auto sparse_payload = std::make_unique<uint16_t[]>(count);
      sparse_payload[0] = FOPCODE_SPARSE_SWITCH;
      sparse_payload[1] = targets.size();
      uint32_t* spkeys = (uint32_t*)&sparse_payload[2];
      uint32_t* sptargets =
          (uint32_t*)&sparse_payload[2 + (targets.size() * 2)];
      for (BranchTarget* target : targets) {
        *spkeys++ = target->case_key;
        *sptargets++ = multi_targets[target] - entry_to_addr.at(multiopcode);
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(DOPCODE_NOP);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop =
          new DexOpcodeData(sparse_payload.get(), (int)(count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      multi_insn->set_offset(addr - entry_to_addr.at(multiopcode));
      multi_insn->set_opcode(DOPCODE_SPARSE_SWITCH);
      addr += count;
    } else {
      // Emit packed.
      const size_t count = (targets.size() * 2) + 4;
      auto packed_payload = std::make_unique<uint16_t[]>(count);
      packed_payload[0] = FOPCODE_PACKED_SWITCH;
      packed_payload[1] = targets.size();
      uint32_t* psdata = (uint32_t*)&packed_payload[2];
      *psdata++ = targets.front()->case_key;
      for (BranchTarget* target : targets) {
        *psdata++ = multi_targets[target] - entry_to_addr.at(multiopcode);
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(DOPCODE_NOP);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop =
          new DexOpcodeData(packed_payload.get(), (int)(count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      multi_insn->set_offset(addr - entry_to_addr.at(multiopcode));
      multi_insn->set_opcode(DOPCODE_PACKED_SWITCH);
      addr += count;
    }
  }

  // Step 4, emit debug entries
  TRACE(MTRANS, 5, "Emitting debug entries\n");
  auto debugitem = code->get_debug_item();
  if (debugitem) {
    gather_debug_entries(m_ir_list, entry_to_addr, &debugitem->get_entries());
  }
  // Step 5, try/catch blocks
  TRACE(MTRANS, 5, "Emitting try items & catch handlers\n");
  auto& tries = code->get_tries();
  tries.clear();
  MethodItemEntry* active_try = nullptr;
  for (auto& mentry : *m_ir_list) {
    if (mentry.type != MFLOW_TRY) {
      continue;
    }
    auto& tentry = mentry.tentry;
    if (tentry->type == TRY_START) {
      always_assert(active_try == nullptr);
      active_try = &mentry;
      continue;
    }
    redex_assert(tentry->type == TRY_END);
    auto try_end = &mentry;
    auto try_start = active_try;
    active_try = nullptr;

    always_assert_log(
        try_start != nullptr, "unopened try_end found: %s", SHOW(*try_end));
    always_assert_log(try_start->tentry->catch_start ==
                          try_end->tentry->catch_start,
                      "mismatched try start (%s) and end (%s)",
                      SHOW(*try_start),
                      SHOW(*try_end));
    auto start_addr = entry_to_addr.at(try_start);
    auto end_addr = entry_to_addr.at(try_end);
    if (start_addr == end_addr) {
      continue;
    }

    DexCatches catches;
    for (auto mei = try_end->tentry->catch_start;
        mei != nullptr;
        mei = mei->centry->next) {
      if (mei->centry->next != nullptr) {
        always_assert(mei->centry->catch_type != nullptr);
      }
      catches.emplace_back(mei->centry->catch_type, entry_to_addr.at(mei));
    }
    split_and_insert_try_regions(start_addr, end_addr, catches, &tries);
  }
  always_assert_log(active_try == nullptr, "unclosed try_start found");

  std::sort(tries.begin(),
            tries.end(),
            [](const std::unique_ptr<DexTryItem>& a,
               const std::unique_ptr<DexTryItem>& b) {
              return a->m_start_addr < b->m_start_addr;
            });
  return true;
}
