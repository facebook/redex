/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Transform.h"

#include <algorithm>
#include <memory>

#include "Debug.h"
#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "WorkQueue.h"

////////////////////////////////////////////////////////////////////////////////

std::mutex MethodTransform::s_lock;
MethodTransform::FatMethodCache MethodTransform::s_cache;

////////////////////////////////////////////////////////////////////////////////

void PostOrderSort::postorder(Block* b) {
  auto pos = m_visited.find(b);
  if (pos != m_visited.end()) {
    return;
  }
  m_visited.emplace_hint(pos, b);
  for (auto& s : b->succs()) {
    postorder(s);
  }
  m_postorder_list.emplace_back(b);
}

std::vector<Block*>&& PostOrderSort::get() {
  if (m_cfg.size() > 0) {
    postorder(m_cfg[0]);
    for (size_t i = 1; i < m_cfg.size(); i++) {
      if (m_cfg[i]->preds().size() == 0) postorder(m_cfg[i]);
    }
  }
  return std::move(m_postorder_list);
}

////////////////////////////////////////////////////////////////////////////////

MethodTransform::~MethodTransform() {
  m_fmethod->clear_and_dispose(FatMethodDisposer());
  delete m_fmethod;
  for (auto block : m_blocks) {
    delete block;
  }
}

MethodTransform* MethodTransform::get_method_transform(
    DexMethod* method,
    bool want_cfg /* = false */
) {
  {
    std::lock_guard<std::mutex> g(s_lock);
    auto it = s_cache.find(method);
    if (it != s_cache.end()) {
      MethodTransform* mt = it->second;
      return mt;
    }
  }
  FatMethod* fm = balloon(method);
  MethodTransform* mt = new MethodTransform(method, fm);
  if (want_cfg) {
    mt->build_cfg();
  }
  {
    std::lock_guard<std::mutex> g(s_lock);
    s_cache[method] = mt;
    return mt;
  }
}

MethodTransform* MethodTransform::get_new_method(DexMethod* method) {
  return new MethodTransform(method, new FatMethod());
}

namespace {
typedef std::unordered_map<uint32_t, MethodItemEntry*> addr_mei_t;

struct EncodeResult {
  bool success;
  DexOpcode newopcode;
};

EncodeResult encode_offset(DexInstruction* insn, int32_t offset) {
  int bytecount = 4;
  if ((int32_t)((int8_t)(offset & 0xff)) == offset) {
    bytecount = 1;
  } else if ((int32_t)((int16_t)(offset & 0xffff)) == offset) {
    bytecount = 2;
  }

  auto op = insn->opcode();
  if (is_conditional_branch(op)) {
    always_assert_log(bytecount <= 2,
                      "Overflowed 16-bit encoding for offset in %s",
                      SHOW(insn));
  }
  if (is_goto(op)) {
    // Use the smallest encoding possible.
    auto newopcode = [&] {
      // branch offset 0 must use goto/32, as per the spec.
      if (offset == 0) {
        return OPCODE_GOTO_32;
      }
      switch (bytecount) {
      case 1:
        return OPCODE_GOTO;
      case 2:
        return OPCODE_GOTO_16;
      case 4:
        return OPCODE_GOTO_32;
      }
      always_assert_log(false, "Invalid bytecount for %s", SHOW(insn));
    }();

    if (newopcode != op) {
      return {false, newopcode};
    }
  }
  insn->set_offset(offset);
  return {true, op};
}

static MethodItemEntry* get_target(MethodItemEntry* mei,
                                   addr_mei_t& addr_to_mei) {
  uint32_t base = mei->addr;
  int offset = mei->insn->offset();
  uint32_t target = base + offset;
  always_assert_log(
      addr_to_mei.count(target) != 0,
      "Invalid opcode target %08x[%p](%08x) %08x in get_target %s\n",
      base,
      mei,
      offset,
      target,
      SHOW(mei->insn));
  return addr_to_mei[target];
}

static void insert_mentry_before(FatMethod* fm,
                                 MethodItemEntry* mentry,
                                 MethodItemEntry* dest) {
  if (dest == nullptr) {
    fm->push_back(*mentry);
  } else {
    fm->insert(fm->iterator_to(*dest), *mentry);
  }
}

static void insert_branch_target(FatMethod* fm,
                                 MethodItemEntry* target,
                                 MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget();
  bt->type = BRANCH_SIMPLE;
  bt->src = src;

  MethodItemEntry* mentry = new MethodItemEntry(bt);
  ;
  insert_mentry_before(fm, mentry, target);
}

static void insert_fallthrough(FatMethod* fm, MethodItemEntry* dest) {
  MethodItemEntry* fallthrough = new MethodItemEntry();
  insert_mentry_before(fm, fallthrough, dest);
}

static bool multi_target_compare_index(const BranchTarget* a,
                                       const BranchTarget* b) {
  return (a->index < b->index);
}

static bool multi_contains_gaps(const std::vector<BranchTarget*>& targets) {
  int32_t key = targets.front()->index;
  for (auto target : targets) {
    if (target->index != key) return true;
    key++;
  }
  return false;
}

static void insert_multi_branch_target(FatMethod* fm,
                                       int32_t index,
                                       MethodItemEntry* target,
                                       MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget();
  bt->type = BRANCH_MULTI;
  bt->src = src;
  bt->index = index;

  MethodItemEntry* mentry = new MethodItemEntry(bt);
  insert_mentry_before(fm, mentry, target);
}

static int32_t read_int32(const uint16_t*& data) {
  int32_t result;
  memcpy(&result, data, sizeof(int32_t));
  data += 2;
  return result;
}

static void shard_multi_target(FatMethod* fm,
                               DexOpcodeData* fopcode,
                               MethodItemEntry* src,
                               addr_mei_t& addr_to_mei) {
  const uint16_t* data = fopcode->data();
  uint16_t entries = *data++;
  auto ftype = fopcode->opcode();
  uint32_t base = src->addr;
  if (ftype == FOPCODE_PACKED_SWITCH) {
    int32_t index = read_int32(data);
    for (int i = 0; i < entries; i++) {
      uint32_t targetaddr = base + read_int32(data);
      auto target = addr_to_mei[targetaddr];
      insert_multi_branch_target(fm, index, target, src);
      index++;
    }
  } else if (ftype == FOPCODE_SPARSE_SWITCH) {
    const uint16_t* tdata = data + 2 * entries;  // entries are 32b
    for (int i = 0; i < entries; i++) {
      int32_t index = read_int32(data);
      uint32_t targetaddr = base + read_int32(tdata);
      auto target = addr_to_mei[targetaddr];
      insert_multi_branch_target(fm, index, target, src);
    }
  } else {
    always_assert_log(false, "Bad fopcode 0x%04x in shard_multi_target", ftype);
  }
}

static void generate_branch_targets(FatMethod* fm, addr_mei_t& addr_to_mei) {
  for (auto miter = fm->begin(); miter != fm->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE) {
      auto insn = mentry->insn;
      if (is_branch(insn->opcode())) {
        auto target = get_target(mentry, addr_to_mei);
        if (is_multi_branch(insn->opcode())) {
          auto fopcode = static_cast<DexOpcodeData*>(target->insn);
          shard_multi_target(fm, fopcode, mentry, addr_to_mei);
          fm->erase(fm->iterator_to(*target));
          delete fopcode;
          addr_to_mei.erase(target->addr);
        } else {
          insert_branch_target(fm, target, mentry);
        }
      }
    }
  }
}

static void associate_debug_entries(FatMethod* fm,
                                    DexDebugItem* dbg,
                                    addr_mei_t& addr_to_mei) {
  for (auto& entry : dbg->get_entries()) {
    auto insert_point = addr_to_mei[entry.addr];
    if (!insert_point) {
      /* We don't have a way of emitting debug info for fopcodes.  To
       * be honest, I'm not sure why DX emits them.  We don't.
       */
      TRACE(MTRANS, 5, "Warning..Skipping fopcode debug opcode\n");
      continue;
    }
    MethodItemEntry* mentry;
    switch (entry.type) {
      case DexDebugEntryType::Instruction:
        mentry = new MethodItemEntry(entry.insn);
        break;
      case DexDebugEntryType::Position:
        mentry = new MethodItemEntry(entry.pos);
        break;
    }
    insert_mentry_before(fm, mentry, insert_point);
  }
}

static void associate_try_items(FatMethod* fm,
                                DexCode* code,
                                addr_mei_t& addr_to_mei) {
  auto const& tries = code->get_tries();
  for (auto& tri : tries) {
    MethodItemEntry* catch_start = nullptr;
    CatchEntry* last_catch = nullptr;
    for (auto catz : tri->m_catches) {
      auto catzop = addr_to_mei[catz.second];
      TRACE(MTRANS, 3, "try_catch %08x mei %p\n", catz.second, catzop);
      auto catch_mei = new MethodItemEntry(catz.first);
      catch_start = catch_start == nullptr ? catch_mei : catch_start;
      if (last_catch != nullptr) {
        last_catch->next = catch_mei;
      }
      last_catch = catch_mei->centry;
      insert_mentry_before(fm, catch_mei, catzop);
    }

    auto begin = addr_to_mei[tri->m_start_addr];
    TRACE(MTRANS, 3, "try_start %08x mei %p\n", tri->m_start_addr, begin);
    auto try_start = new MethodItemEntry(TRY_START, catch_start);
    insert_mentry_before(fm, try_start, begin);
    uint32_t lastaddr = tri->m_start_addr + tri->m_insn_count;
    auto end = addr_to_mei[lastaddr];
    TRACE(MTRANS, 3, "try_end %08x mei %p\n", lastaddr, end);
    auto try_end = new MethodItemEntry(TRY_END, catch_start);
    insert_mentry_before(fm, try_end, end);
  }
}
}

FatMethod* MethodTransform::balloon(DexMethod* method) {
  auto code = method->get_code();
  if (code == nullptr) {
    return nullptr;
  }
  TRACE(MTRANS, 2, "Ballooning %s\n", SHOW(method));
  auto opcodes = code->get_instructions();
  addr_mei_t addr_to_mei;

  FatMethod* fm = new FatMethod();
  uint32_t addr = 0;
  for (auto opcode : opcodes) {
    MethodItemEntry* mei = new MethodItemEntry(opcode);
    fm->push_back(*mei);
    addr_to_mei[addr] = mei;
    mei->addr = addr;
    TRACE(MTRANS, 5, "%08x: %s[mei %p]\n", addr, SHOW(opcode), mei);
    addr += opcode->size();
  }
  generate_branch_targets(fm, addr_to_mei);
  associate_try_items(fm, code, addr_to_mei);
  auto debugitem = code->get_debug_item();
  if (debugitem) {
    associate_debug_entries(fm, debugitem, addr_to_mei);
  }
  return fm;
}

void MethodTransform::replace_opcode(DexInstruction* from, DexInstruction* to) {
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE && mentry->insn == from) {
      mentry->insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s' in method %s",
      SHOW(from),
      SHOW(to),
      show_short(m_method).c_str());
}

void MethodTransform::insert_after(DexInstruction* position,
                                   std::list<DexInstruction*>& opcodes) {
  /* The nullptr case handling is strange-ish..., this will not work as expected
   *if
   * a method has a branch target as it's first instruction.
   *
   * To handle this case sanely, we'd need to export a interface based on
   * MEI's probably.
   *
   */
  for (auto const& mei : *m_fmethod) {
    if (mei.type == MFLOW_OPCODE &&
        (position == nullptr || mei.insn == position)) {
      auto insertat = m_fmethod->iterator_to(mei);
      if (position != nullptr) insertat++;
      for (auto opcode : opcodes) {
        MethodItemEntry* mentry = new MethodItemEntry(opcode);
        m_fmethod->insert(insertat, *mentry);
      }
      return;
    }
  }
  always_assert_log(false, "No match found");
}

void MethodTransform::remove_opcode(DexInstruction* insn) {
  for (auto const& mei : *m_fmethod) {
    if (mei.type == MFLOW_OPCODE && mei.insn == insn) {
      m_fmethod->erase(m_fmethod->iterator_to(mei));
      delete insn;
      return;
    }
  }
  always_assert_log(false,
                    "No match found while removing '%s' from method %s",
                    SHOW(insn),
                    show_short(m_method).c_str());
}

FatMethod::iterator MethodTransform::main_block() { return m_fmethod->begin(); }

FatMethod::iterator MethodTransform::insert(FatMethod::iterator cur,
                                            DexInstruction* insn) {
  MethodItemEntry* mentry = new MethodItemEntry(insn);
  return m_fmethod->insert(cur, *mentry);
}

FatMethod::iterator MethodTransform::make_if_block(
    FatMethod::iterator cur,
    DexInstruction* insn,
    FatMethod::iterator* false_block) {
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_fmethod->insert(cur, *if_entry);
  auto bt = new BranchTarget();
  bt->src = if_entry;
  bt->type = BRANCH_SIMPLE;
  auto bentry = new MethodItemEntry(bt);
  return m_fmethod->insert(m_fmethod->end(), *bentry);
}

FatMethod::iterator MethodTransform::make_if_else_block(
    FatMethod::iterator cur,
    DexInstruction* insn,
    FatMethod::iterator* false_block,
    FatMethod::iterator* true_block) {
  // if block
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_fmethod->insert(cur, *if_entry);

  // end of else goto
  auto goto_entry = new MethodItemEntry(new DexInstruction(OPCODE_GOTO));
  auto goto_it = m_fmethod->insert(m_fmethod->end(), *goto_entry);

  // main block
  auto main_bt = new BranchTarget();
  main_bt->src = goto_entry;
  main_bt->type = BRANCH_SIMPLE;
  auto mb_entry = new MethodItemEntry(main_bt);
  auto main_block = m_fmethod->insert(goto_it, *mb_entry);

  // else block
  auto else_bt = new BranchTarget();
  else_bt->src = if_entry;
  else_bt->type = BRANCH_SIMPLE;
  auto eb_entry = new MethodItemEntry(else_bt);
  *true_block = m_fmethod->insert(goto_it, *eb_entry);

  return main_block;
}

FatMethod::iterator MethodTransform::make_switch_block(
    FatMethod::iterator cur,
    DexInstruction* insn,
    FatMethod::iterator* default_block,
    std::map<int, FatMethod::iterator>& cases) {
  auto switch_entry = new MethodItemEntry(insn);
  *default_block = m_fmethod->insert(cur, *switch_entry);
  FatMethod::iterator main_block = *default_block;
  for (auto case_it = cases.begin(); case_it != cases.end(); ++case_it) {
    auto goto_entry = new MethodItemEntry(new DexInstruction(OPCODE_GOTO));
    auto goto_it = m_fmethod->insert(m_fmethod->end(), *goto_entry);

    auto main_bt = new BranchTarget();
    main_bt->src = goto_entry;
    main_bt->type = BRANCH_SIMPLE;
    auto mb_entry = new MethodItemEntry(main_bt);
    main_block = m_fmethod->insert(++main_block, *mb_entry);

    // case block
    auto case_bt = new BranchTarget();
    case_bt->src = switch_entry;
    case_bt->index = case_it->first;
    case_bt->type = BRANCH_MULTI;
    auto eb_entry = new MethodItemEntry(case_bt);
    case_it->second = m_fmethod->insert(goto_it, *eb_entry);
  }
  return main_block;
}

namespace {
using RegMap = std::unordered_map<uint16_t, uint16_t>;

void remap_dest(DexInstruction* inst, const RegMap& reg_map) {
  if (!inst->dests_size()) return;
  if (inst->dest_is_src()) return;
  auto it = reg_map.find(inst->dest());
  if (it == reg_map.end()) return;
  inst->set_dest(it->second);
}

void remap_srcs(DexInstruction* inst, const RegMap& reg_map) {
  for (unsigned i = 0; i < inst->srcs_size(); i++) {
    auto it = reg_map.find(inst->src(i));
    if (it == reg_map.end()) continue;
    inst->set_src(i, it->second);
  }
}

void remap_debug(DexDebugInstruction* dbgop, const RegMap& reg_map) {
  switch (dbgop->opcode()) {
  case DBG_START_LOCAL:
  case DBG_START_LOCAL_EXTENDED:
  case DBG_END_LOCAL:
  case DBG_RESTART_LOCAL: {
    auto it = reg_map.find(dbgop->uvalue());
    if (it == reg_map.end()) return;
    dbgop->set_uvalue(it->second);
    break;
  }
  default:
    break;
  }
}

void remap_registers(DexInstruction* insn, const RegMap& reg_map) {
  remap_dest(insn, reg_map);
  remap_srcs(insn, reg_map);
}

void remap_registers(MethodItemEntry& mei, const RegMap& reg_map) {
  switch (mei.type) {
  case MFLOW_OPCODE:
    remap_registers(mei.insn, reg_map);
    break;
  case MFLOW_DEBUG:
    remap_debug(mei.dbgop, reg_map);
    break;
  default:
    break;
  }
}

void remap_registers(FatMethod* fmethod, const RegMap& reg_map) {
  for (auto& mei : *fmethod) {
    remap_registers(mei, reg_map);
  }
}

void remap_caller_regs(DexMethod* method,
                       FatMethod* fmethod,
                       uint16_t newregs) {
  RegMap reg_map;
  auto oldregs = method->get_code()->get_registers_size();
  auto ins = method->get_code()->get_ins_size();
  for (uint16_t i = 0; i < ins; ++i) {
    reg_map[oldregs - ins + i] = newregs - ins + i;
  }
  remap_registers(fmethod, reg_map);
  method->get_code()->set_registers_size(newregs);
}

void remap_callee_regs(DexInstruction* invoke,
                       DexMethod* method,
                       FatMethod* fmethod,
                       uint16_t newregs) {
  RegMap reg_map;
  auto oldregs = method->get_code()->get_registers_size();
  auto ins = method->get_code()->get_ins_size();
  auto wc = invoke->arg_word_count();
  always_assert(ins == wc);
  for (uint16_t i = 0; i < wc; ++i) {
    reg_map[oldregs - ins + i] = invoke->src(i);
  }
  remap_registers(fmethod, reg_map);
  method->get_code()->set_registers_size(newregs);
}

/**
 * Builds a register map for a callee.
 */
void build_remap_regs(RegMap& reg_map,
                      DexInstruction* invoke,
                      DexMethod* callee,
                      uint16_t new_tmp_off) {
  auto oldregs = callee->get_code()->get_registers_size();
  auto ins = callee->get_code()->get_ins_size();
  // remap all local regs (not args)
  for (uint16_t i = 0; i < oldregs - ins; ++i) {
    reg_map[i] = new_tmp_off + i;
  }
  if (is_invoke_range(invoke->opcode())) {
    auto base = invoke->range_base();
    auto range = invoke->range_size();
    always_assert(ins == range);
    for (uint16_t i = 0; i < range; ++i) {
      reg_map[oldregs - ins + i] = base + i;
    }
  } else {
    auto wc = invoke->arg_word_count();
    always_assert(ins == wc);
    for (uint16_t i = 0; i < wc; ++i) {
      reg_map[oldregs - ins + i] = invoke->src(i);
    }
  }
}

/**
 * Create a move instruction given a return instruction in a callee and
 * a move-result instruction in a caller.
 */
DexInstruction* move_result(DexInstruction* res, DexInstruction* move_res) {
  auto opcode = res->opcode();
  always_assert(opcode != OPCODE_RETURN_VOID);
  DexInstruction* move;
  if (opcode == OPCODE_RETURN_OBJECT) {
    move = new DexInstruction(OPCODE_MOVE_OBJECT);
  } else if (opcode == OPCODE_RETURN_WIDE) {
    move = new DexInstruction(OPCODE_MOVE_WIDE);
  } else {
    always_assert(opcode == OPCODE_RETURN);
    move = new DexInstruction(OPCODE_MOVE);
  }
  move->set_dest(move_res->dest());
  move->set_src(0, res->src(0));
  return move;
}

/* We need to cleanup two cases:
 * Duplicate DBG_SET_PROLOGUE_END
 * Uninitialized parameters
 *
 * The parameter names are part of the debug info for the method.
 * The technically correct solution would be to make a start
 * local for each of them.  However, that would also imply another
 * end local after the tail to correctly set what the register
 * is at the end.  This would bloat the debug info parameters for
 * a corner case.
 *
 * Instead, we just delete locals lifetime information for parameters.
 * This is an exceedingly rare case triggered by goofy code that
 * reuses parameters as locals.
 */
void cleanup_callee_debug(FatMethod* fcallee) {
  std::unordered_set<uint16_t> valid_regs;
  auto it = fcallee->begin();
  while (it != fcallee->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_DEBUG) {
      switch(mei.dbgop->opcode()) {
      case DBG_SET_PROLOGUE_END:
        fcallee->erase(fcallee->iterator_to(mei));
        break;
      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED: {
        auto reg = mei.dbgop->uvalue();
        valid_regs.insert(reg);
        break;
      }
      case DBG_END_LOCAL:
      case DBG_RESTART_LOCAL: {
        auto reg = mei.dbgop->uvalue();
        if (valid_regs.find(reg) == valid_regs.end()) {
          fcallee->erase(fcallee->iterator_to(mei));
        }
        break;
      }
      default:
        break;
      }
    }
  }
}

/*
 * For splicing a callee's FatMethod into a caller.
 */
class MethodSplicer {
  // We need a map of MethodItemEntry we have created because a branch
  // points to another MethodItemEntry which may have been created or not
  std::unordered_map<MethodItemEntry*, MethodItemEntry*> m_entry_map;
  // for remapping the parent position pointers
  std::unordered_map<DexPosition*, DexPosition*> m_pos_map;
  const RegMap& m_callee_reg_map;
  DexPosition* m_invoke_position;

 public:
  MethodSplicer(const RegMap& callee_reg_map, DexPosition* invoke_position)
      : m_callee_reg_map(callee_reg_map), m_invoke_position(invoke_position) {
    m_entry_map[nullptr] = nullptr;
    m_pos_map[nullptr] = nullptr;
  }

 private:
  MethodItemEntry* clone(MethodItemEntry* mei) {
    MethodItemEntry* cloned_mei;
    auto entry = m_entry_map.find(mei);
    if (entry != m_entry_map.end()) {
      return entry->second;
    }
    cloned_mei = new MethodItemEntry(*mei);
    m_entry_map[mei] = cloned_mei;
    switch (cloned_mei->type) {
    case MFLOW_TRY:
      cloned_mei->tentry = new TryEntry(*cloned_mei->tentry);
      cloned_mei->tentry->catch_start = clone(cloned_mei->tentry->catch_start);
      return cloned_mei;
    case MFLOW_CATCH:
      cloned_mei->centry = new CatchEntry(*cloned_mei->centry);
      cloned_mei->centry->next = clone(cloned_mei->centry->next);
      return cloned_mei;
    case MFLOW_OPCODE:
      cloned_mei->insn = cloned_mei->insn->clone();
      return cloned_mei;
    case MFLOW_TARGET:
      cloned_mei->target = new BranchTarget(*cloned_mei->target);
      cloned_mei->target->src = clone(cloned_mei->target->src);
      return cloned_mei;
    case MFLOW_DEBUG:
      // XXX MethodItemEntry doesn't delete dbgop in its dtor, so this is
      // probably
      // a leak
      cloned_mei->dbgop = cloned_mei->dbgop->clone();
      return cloned_mei;
    case MFLOW_POSITION:
      // XXX MethodItemEntry doesn't delete pos in its dtor, so this is probably
      // a leak
      cloned_mei->pos = new DexPosition(*cloned_mei->pos);
      m_pos_map[mei->pos] = cloned_mei->pos;
      cloned_mei->pos->parent = m_pos_map.at(cloned_mei->pos->parent);
      return cloned_mei;
    case MFLOW_FALLTHROUGH:
      return cloned_mei;
    }
    not_reached();
  }

 public:
  void operator()(FatMethod* fcaller,
                  FatMethod::iterator insert_pos,
                  FatMethod::iterator fcallee_start,
                  FatMethod::iterator fcallee_end) {
    auto it = fcallee_start;
    while (it != fcallee_end) {
      auto mei = clone(&*it);
      remap_registers(*mei, m_callee_reg_map);
      it++;
      if (mei->type == MFLOW_POSITION && mei->pos->parent == nullptr) {
        mei->pos->parent = m_invoke_position;
      }
      fcaller->insert(insert_pos, *mei);
    }
  }
};
}

void MethodTransform::inline_tail_call(DexMethod* caller,
                                       DexMethod* callee,
                                       DexInstruction* invoke) {
  TRACE(INL, 2, "caller: %s\ncallee: %s\n", SHOW(caller), SHOW(callee));
  MethodTransformer tcaller(caller);
  MethodTransformer tcallee(callee);
  auto fcaller = tcaller->m_fmethod;
  auto fcallee = tcallee->m_fmethod;

  auto bregs = caller->get_code()->get_registers_size();
  auto eregs = callee->get_code()->get_registers_size();
  auto bins = caller->get_code()->get_ins_size();
  auto eins = callee->get_code()->get_ins_size();
  always_assert(bins >= eins);
  auto newregs = std::max(bregs, uint16_t(eregs + bins - eins));
  always_assert(newregs <= 16);

  // Remap registers to account for possibly larger frame, more ins
  remap_caller_regs(caller, fcaller, newregs);
  remap_callee_regs(invoke, callee, fcallee, newregs);

  callee->get_code()->set_ins_size(bins);

  auto pos = std::find_if(fcaller->begin(),
                          fcaller->end(),
                          [invoke](const MethodItemEntry& mei) {
                            return mei.type == MFLOW_OPCODE && mei.insn == invoke;
                          });

  cleanup_callee_debug(fcallee);
  auto it = fcallee->begin();
  while (it != fcallee->end()) {
    auto& mei = *it++;
    fcallee->erase(fcallee->iterator_to(mei));
    fcaller->insert(pos, mei);
  }
  // Delete the vestigial tail.
  while (pos != fcaller->end()) {
    if (pos->type == MFLOW_OPCODE) {
      pos = fcaller->erase_and_dispose(pos, FatMethodDisposer());
    } else {
      ++pos;
    }
  }

  caller->get_code()->set_outs_size(callee->get_code()->get_outs_size());
}

bool MethodTransform::inline_16regs(InlineContext& context,
                                    DexMethod *callee,
                                    DexOpcodeMethod *invoke) {
  TRACE(INL, 2, "caller: %s\ncallee: %s\n",
      SHOW(context.caller), SHOW(callee));
  auto caller = context.caller;
  MethodTransformer mtcaller(caller);
  MethodTransformer mtcallee(callee);
  auto fcaller = mtcaller->m_fmethod;
  auto fcallee = mtcallee->m_fmethod;

  auto callee_code = callee->get_code();
  auto temps_needed =
      callee_code->get_registers_size() - callee_code->get_ins_size();
  uint16_t newregs = caller->get_code()->get_registers_size();
  if (context.inline_regs_used < temps_needed) {
    newregs = newregs + temps_needed - context.inline_regs_used;
    if (newregs > 16) return false;
    remap_caller_regs(caller, fcaller, newregs);
    context.inline_regs_used = temps_needed;
  }
  RegMap callee_reg_map;
  build_remap_regs(callee_reg_map, invoke, callee, context.new_tmp_off);

  auto pos = std::find_if(
    fcaller->begin(), fcaller->end(),
    [invoke](const MethodItemEntry& mei) {
      return mei.type == MFLOW_OPCODE && mei.insn == invoke;
    });
  // find the move-result after the invoke, if any. Must be the first
  // instruction after the invoke
  auto move_res = pos;
  while (move_res++ != fcaller->end() && move_res->type != MFLOW_OPCODE);
  if (!is_move_result(move_res->insn->opcode())) {
    move_res = fcaller->end();
  }

  // Skip dbg prologue in callee, we don't need two.
  auto it = fcallee->begin();
  if (it->type == MFLOW_DEBUG && it->dbgop->opcode() == DBG_SET_PROLOGUE_END) {
    ++it;
  }
  // find the last position entry before the invoke.
  // we need to decrement the reverse iterator because it gets constructed
  // as pointing to the element preceding pos
  auto position_it = --FatMethod::reverse_iterator(pos);
  while (++position_it != fcaller->rend()
      && position_it->type != MFLOW_POSITION);
  auto invoke_position =
    position_it == fcaller->rend() ? nullptr : position_it->pos;
  if (invoke_position != nullptr) {
    TRACE(MTRANS, 5, "Inlining call at %s:%d\n", invoke_position->file->c_str(),
        invoke_position->line);
  }

  // check if we are in a try block
  auto try_it = pos;
  while (++try_it != fcaller->end() && try_it->type != MFLOW_TRY);
  auto active_catch = try_it != fcaller->end() && try_it->tentry->type == TRY_END
    ? try_it->tentry->catch_start : nullptr;

  // Copy the callee up to the return. Everything else we push at the end
  // of the caller
  auto splice = MethodSplicer(callee_reg_map, invoke_position);
  auto ret_it =
      std::find_if(it, fcallee->end(), [](const MethodItemEntry& mei) {
        return mei.type == MFLOW_OPCODE && is_return(mei.insn->opcode());
      });
  splice(fcaller, pos, it, ret_it);
  if (move_res != fcaller->end()) {
    std::unique_ptr<DexInstruction> ret_insn(ret_it->insn->clone());
    remap_registers(ret_insn.get(), callee_reg_map);
    DexInstruction* move = move_result(ret_insn.get(), move_res->insn);
    auto move_mei = new MethodItemEntry(move);
    fcaller->insert(pos, *move_mei);
  }
  // ensure that the caller's code after the inlined method retain their
  // original position
  if (invoke_position != nullptr) {
    fcaller->insert(pos, *(new MethodItemEntry(invoke_position)));
  }

  // remove invoke
  fcaller->erase_and_dispose(pos, FatMethodDisposer());
  // remove move_result
  if (move_res != fcaller->end()) {
    fcaller->erase_and_dispose(move_res, FatMethodDisposer());
  }

  if (it != fcallee->end()) {
    if (active_catch != nullptr) {
      fcaller->push_back(*(new MethodItemEntry(TRY_START, active_catch)));
    }
    // Copy the opcodes in the callee after the return and put them at the end
    // of the caller.
    splice(fcaller, fcaller->end(), std::next(ret_it), fcallee->end());
    if (active_catch != nullptr) {
      fcaller->push_back(*(new MethodItemEntry(TRY_END, active_catch)));
    }
  }

  // adjust method header
  caller->get_code()->set_registers_size(newregs);
  caller->get_code()->set_outs_size(
      std::max(callee->get_code()->get_outs_size(),
      caller->get_code()->get_outs_size()));
  return true;
}

namespace {
bool end_of_block(const FatMethod* fm, FatMethod::iterator it, bool in_try) {
  auto next = std::next(it);
  if (next == fm->end()) {
    return true;
  }
  if (next->type == MFLOW_TARGET || next->type == MFLOW_TRY ||
      next->type == MFLOW_CATCH) {
    return true;
  }
  if (it->type != MFLOW_OPCODE) {
    return false;
  }
  if (is_branch(it->insn->opcode())) {
    return true;
  }
  if (in_try && may_throw(it->insn->opcode())) {
    return true;
  }
  return false;
}
}

bool ends_with_may_throw(Block* p) {
  for (auto last = p->rbegin(); last != p->rend(); ++last) {
    if (last->type != MFLOW_OPCODE) {
      continue;
    }
    return may_throw(last->insn->opcode());
  }
  return true;
}

void MethodTransform::build_cfg() {
  // Find the block boundaries
  std::unordered_map<MethodItemEntry*, std::vector<Block*>> branch_to_targets;
  std::vector<std::pair<TryEntry*, Block*>> try_ends;
  std::unordered_map<CatchEntry*, Block*> try_catches;
  size_t id = 0;
  bool in_try = false;
  m_blocks.emplace_back(new Block(id++));
  m_blocks.back()->m_begin = m_fmethod->begin();
  // The first block can be a branch target.
  auto begin = m_fmethod->begin();
  if (begin->type == MFLOW_TARGET) {
    branch_to_targets[begin->target->src].push_back(m_blocks.back());
  }
  for (auto it = m_fmethod->begin(); it != m_fmethod->end(); ++it) {
    if (it->type == MFLOW_TRY) {
      if (it->tentry->type == TRY_START) {
        in_try = true;
      } else if (it->tentry->type == TRY_END) {
        in_try = false;
      }
    }
    if (!end_of_block(m_fmethod, it, in_try)) {
      continue;
    }
    // End the current block.
    auto next = std::next(it);
    if (next == m_fmethod->end()) {
      m_blocks.back()->m_end = next;
      continue;
    }
    // Start a new block at the next MethodItem.
    auto next_block = new Block(id++);
    if (next->type == MFLOW_OPCODE) {
      insert_fallthrough(m_fmethod, &*next);
      next = std::next(it);
    }
    m_blocks.back()->m_end = next;
    next_block->m_begin = next;
    m_blocks.emplace_back(next_block);
    // Record branch targets to add edges in the next pass.
    if (next->type == MFLOW_TARGET) {
      branch_to_targets[next->target->src].push_back(next_block);
      continue;
    }
    // Record try/catch blocks to add edges in the next pass.
    if (next->type == MFLOW_TRY && next->tentry->type == TRY_END) {
      try_ends.emplace_back(next->tentry, next_block);
    } else if (next->type == MFLOW_CATCH) {
      try_catches[next->centry] = next_block;
    }
  }
  // Link the blocks together with edges
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    // Set outgoing edge if last MIE falls through
    auto lastmei = (*it)->rbegin();
    bool fallthrough = true;
    if (lastmei->type == MFLOW_OPCODE) {
      auto lastop = lastmei->insn->opcode();
      if (is_goto(lastop) || is_conditional_branch(lastop) ||
          is_multi_branch(lastop)) {
        fallthrough = !is_goto(lastop);
        auto const& targets = branch_to_targets[&*lastmei];
        for (auto target : targets) {
          (*it)->m_succs.push_back(target);
          target->m_preds.push_back(*it);
        }
      } else if (is_return(lastop) || lastop == OPCODE_THROW) {
        fallthrough = false;
      }
    }
    if (fallthrough && std::next(it) != m_blocks.end()) {
      Block* next = *std::next(it);
      (*it)->m_succs.push_back(next);
      next->m_preds.push_back(*it);
    }
  }
  /*
   * Now add the catch edges.  Every block inside a try-start/try-end region
   * gets an edge to every catch block.  This simplifies dataflow analysis
   * since you can always get the exception state by looking at successors,
   * without any additional analysis.
   *
   * NB: This algorithm assumes that a try-start/try-end region will consist of
   * sequentially-numbered blocks, which is guaranteed because catch regions
   * are contiguous in the bytecode, and we generate blocks in bytecode order.
   */
  for (auto tep : try_ends) {
    auto try_end = tep.first;
    auto tryendblock = tep.second;
    size_t bid = tryendblock->id();
    always_assert(bid > 0);
    --bid;
    while (true) {
      auto block = m_blocks[bid];
      if (ends_with_may_throw(block)) {
        for (auto mei = try_end->catch_start;
             mei != nullptr;
             mei = mei->centry->next) {
          auto catchblock = try_catches.at(mei->centry);
          block->m_succs.push_back(catchblock);
          catchblock->m_preds.push_back(block);
        }
      }
      auto begin = block->begin();
      if (begin->type == MFLOW_TRY) {
        auto tentry = begin->tentry;
        if (tentry->type == TRY_START) {
          always_assert(tentry->catch_start == try_end->catch_start);
          break;
        }
      }
      always_assert_log(bid > 0, "No beginning of try region found");
      --bid;
    }
  }
  TRACE(CFG, 5, "%s\n", show(m_method).c_str());
  TRACE(CFG, 5, "%s", show(m_blocks).c_str());
}

void MethodTransform::sync_all() {
  std::vector<MethodTransform*> transforms;
  for (auto& centry : s_cache) {
    transforms.push_back(centry.second);
  }
  std::vector<WorkItem<MethodTransform>> workitems(transforms.size());
  auto mt_sync = [](MethodTransform* mt) { mt->sync(); };
  for (size_t i = 0; i < transforms.size(); i++) {
    workitems[i].init(mt_sync, transforms[i]);
  }
  WorkQueue wq;

  if (workitems.size() > 0) {
    wq.run_work_items(&workitems[0], (int)workitems.size());
  }
}

void MethodTransform::sync() {
  while (try_sync() == false)
    ;
  {
    std::lock_guard<std::mutex> g(s_lock);
    s_cache.erase(m_method);
  }
  delete this;
}

bool MethodTransform::try_sync() {
  TRACE(MTRANS, 5, "Syncing %s\n", SHOW(m_method));
  auto code = m_method->get_code();
  auto& opout = code->get_instructions();
  opout.clear();
  uint32_t addr = 0;
  addr_mei_t addr_to_mei;
  // Step 1, regenerate opcode list for the method, and
  // and calculate the opcode entries address offsets.
  TRACE(MTRANS, 5, "Emitting opcodes\n");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    TRACE(MTRANS, 5, "Analyzing mentry %p\n", mentry);
    mentry->addr = addr;
    if (mentry->type == MFLOW_OPCODE) {
      if ((mentry->insn->opcode() == FOPCODE_FILLED_ARRAY) && (addr & 1)) {
        opout.push_back(new DexInstruction(OPCODE_NOP));
        ++addr;
      }
      addr_to_mei[addr] = mentry;
      TRACE(MTRANS, 5, "Emitting mentry %p at %08x\n", mentry, addr);
      opout.push_back(mentry->insn);
      addr += mentry->insn->size();
    }
  }
  // Step 2, recalculate branches..., save off multi-branch data.
  TRACE(MTRANS, 5, "Recalculating branches\n");
  std::vector<MethodItemEntry*> multi_branches;
  std::unordered_map<MethodItemEntry*, std::vector<BranchTarget*>> multis;
  std::unordered_map<BranchTarget*, uint32_t> multi_targets;
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE) {
      auto opcode = mentry->insn->opcode();
      if (is_branch(opcode) && is_multi_branch(opcode)) {
        multi_branches.push_back(mentry);
      }
    }
    if (mentry->type == MFLOW_TARGET) {
      BranchTarget* bt = mentry->target;
      if (bt->type == BRANCH_MULTI) {
        multis[bt->src].push_back(bt);
        multi_targets[bt] = mentry->addr;
        // We can't fix the primary switch opcodes address until we emit
        // the fopcode, which comes later.
      } else if (bt->type == BRANCH_SIMPLE) {
        MethodItemEntry* tomutate = bt->src;
        int32_t branchoffset = mentry->addr - tomutate->addr;
        if ((tomutate->insn->opcode() == OPCODE_FILL_ARRAY_DATA) &&
            (mentry->addr & 1)) {
          ++branchoffset; // account for nop spacer
        }
        auto encode_result = encode_offset(tomutate->insn, branchoffset);
        if (!encode_result.success) {
          auto inst = tomutate->insn;
          tomutate->insn = new DexInstruction(encode_result.newopcode);
          delete inst;
          return false;
        }
      }
    }
  }
  TRACE(MTRANS, 5, "Emitting multi-branches\n");
  // Step 3, generate multi-branch fopcodes
  for (auto multiopcode : multi_branches) {
    auto targets = multis[multiopcode];
    std::sort(targets.begin(), targets.end(), multi_target_compare_index);
    if (multi_contains_gaps(targets)) {
      // Emit sparse.
      unsigned long count = (targets.size() * 4) + 2;
      uint16_t sparse_payload[count];
      sparse_payload[0] = FOPCODE_SPARSE_SWITCH;
      sparse_payload[1] = targets.size();
      uint32_t* spkeys = (uint32_t*)&sparse_payload[2];
      uint32_t* sptargets =
          (uint32_t*)&sparse_payload[2 + (targets.size() * 2)];
      for (auto target : targets) {
        *spkeys++ = target->index;
        *sptargets++ = multi_targets[target] - multiopcode->addr;
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(0);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop = new DexOpcodeData(sparse_payload, (int) (count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      encode_offset(multiopcode->insn, addr - multiopcode->addr);
      multiopcode->insn->set_opcode(OPCODE_SPARSE_SWITCH);
      addr += count;
    } else {
      // Emit packed.
      unsigned long count = (targets.size() * 2) + 4;
      uint16_t packed_payload[count];
      packed_payload[0] = FOPCODE_PACKED_SWITCH;
      packed_payload[1] = targets.size();
      uint32_t* psdata = (uint32_t*)&packed_payload[2];
      *psdata++ = targets.front()->index;
      for (auto target : targets) {
        *psdata++ = multi_targets[target] - multiopcode->addr;
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(0);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop = new DexOpcodeData(packed_payload, (int) (count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      encode_offset(multiopcode->insn, addr - multiopcode->addr);
      multiopcode->insn->set_opcode(OPCODE_PACKED_SWITCH);
      addr += count;
    }
  }

  // Step 4, emit debug opcodes
  TRACE(MTRANS, 5, "Emitting debug opcodes\n");
  auto debugitem = code->get_debug_item();
  if (debugitem) {
    auto& entries = debugitem->get_entries();
    entries.clear();
    for (auto& mentry : *m_fmethod) {
      if (mentry.type == MFLOW_DEBUG) {
        entries.emplace_back(mentry.addr, mentry.dbgop->clone());
      } else if (mentry.type == MFLOW_POSITION) {
        entries.emplace_back(mentry.addr, mentry.pos);
      }
    }
  }
  // Step 5, try/catch blocks
  TRACE(MTRANS, 5, "Emitting try items & catch handlers\n");
  auto& tries = code->get_tries();
  tries.clear();
  MethodItemEntry* active_try = nullptr;
  for (auto& mentry : *m_fmethod) {
    if (mentry.type != MFLOW_TRY) {
      continue;
    }
    auto& tentry = mentry.tentry;
    if (tentry->type == TRY_START) {
      always_assert(active_try == nullptr);
      active_try = &mentry;
      continue;
    }
    assert(tentry->type == TRY_END);
    auto try_end = &mentry;
    auto try_start = active_try;
    active_try = nullptr;

    always_assert(try_end->tentry->catch_start == try_start->tentry->catch_start);
    auto insn_count = try_end->addr - try_start->addr;
    if (insn_count == 0) {
      continue;
    }
    auto try_item = new DexTryItem(try_start->addr, insn_count);
    for (auto mei = try_end->tentry->catch_start;
        mei != nullptr;
        mei = mei->centry->next) {
      try_item->m_catches.emplace_back(mei->centry->catch_type, mei->addr);
    }
    tries.emplace_back(try_item);
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
