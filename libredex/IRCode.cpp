/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IRCode.h"

#include <algorithm>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <memory>
#include <unordered_set>
#include <list>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "Transform.h"
#include "Util.h"

MethodItemEntry::MethodItemEntry(const MethodItemEntry& that)
    : type(that.type) {
  switch (type) {
  case MFLOW_TRY:
    tentry = that.tentry;
    break;
  case MFLOW_CATCH:
    centry = that.centry;
    break;
  case MFLOW_OPCODE:
    insn = that.insn;
    break;
  case MFLOW_TARGET:
    target = that.target;
    break;
  case MFLOW_DEBUG:
    new (&dbgop) std::unique_ptr<DexDebugInstruction>(that.dbgop->clone());
    break;
  case MFLOW_POSITION:
    new (&pos) std::unique_ptr<DexPosition>(new DexPosition(*that.pos));
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

MethodItemEntry::~MethodItemEntry() {
  switch (type) {
    case MFLOW_TRY:
      delete tentry;
      break;
    case MFLOW_CATCH:
      delete centry;
      break;
    case MFLOW_TARGET:
      delete target;
      break;
    case MFLOW_DEBUG:
      dbgop.~unique_ptr<DexDebugInstruction>();
      break;
    case MFLOW_POSITION:
      pos.~unique_ptr<DexPosition>();
      break;
    default:
      /* nothing to delete */
      break;
  }
}

void MethodItemEntry::gather_strings(std::vector<DexString*>& lstring) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_strings(lstring);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_strings(lstring);
    break;
  case MFLOW_POSITION:
    // although DexPosition contains strings, these strings don't find their
    // way into the APK
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

void MethodItemEntry::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_methods(lmethod);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_methods(lmethod);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

void MethodItemEntry::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_fields(lfield);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_fields(lfield);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

void MethodItemEntry::gather_types(std::vector<DexType*>& ltype) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    if (centry->catch_type != nullptr) {
      ltype.push_back(centry->catch_type);
    }
    break;
  case MFLOW_OPCODE:
    insn->gather_types(ltype);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_types(ltype);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

////////////////////////////////////////////////////////////////////////////////

IRCode::~IRCode() {
  m_fmethod->clear_and_dispose(FatMethodDisposer());
  delete m_fmethod;
}

boost::sub_range<FatMethod> IRCode::get_param_instructions() const {
  auto params_end = std::find_if_not(
      m_fmethod->begin(), m_fmethod->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });
  return boost::sub_range<FatMethod>(m_fmethod->begin(), params_end);
}

void IRCode::gather_catch_types(std::vector<DexType*>& ltype) const {
  for (auto& mie : *m_fmethod) {
    if (mie.type != MFLOW_CATCH) {
      continue;
    }
    if (mie.centry->catch_type != nullptr) {
      ltype.push_back(mie.centry->catch_type);
    }
  }
}

void IRCode::gather_types(std::vector<DexType*>& ltype) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_types(ltype);
  }
  if (m_dbg) m_dbg->gather_types(ltype);
}

void IRCode::gather_strings(std::vector<DexString*>& lstring) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_strings(lstring);
  }
  if (m_dbg) m_dbg->gather_strings(lstring);
}

void IRCode::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_fields(lfield);
  }
}

void IRCode::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_methods(lmethod);
  }
}

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
    return OPCODE_GOTO_32;
  }
  switch (bytecount(offset)) {
  case 1:
    return OPCODE_GOTO;
  case 2:
    return OPCODE_GOTO_16;
  case 4:
    return OPCODE_GOTO_32;
  default:
    always_assert_log(false, "Invalid bytecount %d", offset);
  }
}

DexOpcode invert_conditional_branch(DexOpcode op) {
  switch (op) {
  case OPCODE_IF_EQ:
    return OPCODE_IF_NE;
  case OPCODE_IF_NE:
    return OPCODE_IF_EQ;
  case OPCODE_IF_LT:
    return OPCODE_IF_GE;
  case OPCODE_IF_GE:
    return OPCODE_IF_LT;
  case OPCODE_IF_GT:
    return OPCODE_IF_LE;
  case OPCODE_IF_LE:
    return OPCODE_IF_GT;
  case OPCODE_IF_EQZ:
    return OPCODE_IF_NEZ;
  case OPCODE_IF_NEZ:
    return OPCODE_IF_EQZ;
  case OPCODE_IF_LTZ:
    return OPCODE_IF_GEZ;
  case OPCODE_IF_GEZ:
    return OPCODE_IF_LTZ;
  case OPCODE_IF_GTZ:
    return OPCODE_IF_LEZ;
  case OPCODE_IF_LEZ:
    return OPCODE_IF_GTZ;
  default:
    always_assert_log(false, "Invalid conditional opcode %s", SHOW(op));
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
  int offset = mei->insn->offset();
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

static void insert_branch_target(FatMethod* fm,
                                 MethodItemEntry* target,
                                 MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget();
  bt->type = BRANCH_SIMPLE;
  bt->src = src;

  fm->insert(fm->iterator_to(*target), *(new MethodItemEntry(bt)));
}

// Returns true if the offset could be encoded without modifying fm.
bool encode_offset(FatMethod* fm,
                   MethodItemEntry* branch_op_mie,
                   int32_t offset) {
  DexOpcode bop = branch_op_mie->insn->opcode();
  if (is_goto(bop)) {
    DexOpcode goto_op = goto_for_offset(offset);
    if (goto_op != bop) {
      auto insn = branch_op_mie->insn;
      branch_op_mie->insn = new IRInstruction(goto_op);
      delete insn;
      return false;
    }
  } else if (is_conditional_branch(bop)) {
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
      auto insn = branch_op_mie->insn;
      branch_op_mie->insn = new IRInstruction(OPCODE_GOTO_32);

      DexOpcode inverted = invert_conditional_branch(bop);
      MethodItemEntry* mei = new MethodItemEntry(new IRInstruction(inverted));
      mei->insn->set_src(0, insn->src(0));
      fm->insert(fm->iterator_to(*branch_op_mie), *mei);

      // this iterator should always be valid -- an if-* instruction cannot
      // be the last opcode in a well-formed method
      auto next_insn_it = std::next(fm->iterator_to(*branch_op_mie));
      insert_branch_target(fm, &*next_insn_it, mei);

      delete insn;
      return false;
    }
  } else {
    always_assert_log(false, "Unexpected opcode %s", SHOW(*branch_op_mie));
  }
  branch_op_mie->insn->set_offset(offset);
  return true;
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

  fm->insert(fm->iterator_to(*target), *(new MethodItemEntry(bt)));
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
                               const EntryAddrBiMap& bm) {
  const uint16_t* data = fopcode->data();
  uint16_t entries = *data++;
  auto ftype = fopcode->opcode();
  uint32_t base = bm.by<Entry>().at(src);
  if (ftype == FOPCODE_PACKED_SWITCH) {
    int32_t index = read_int32(data);
    for (int i = 0; i < entries; i++) {
      uint32_t targetaddr = base + read_int32(data);
      auto target = bm.by<Addr>().at(targetaddr);
      insert_multi_branch_target(fm, index, target, src);
      index++;
    }
  } else if (ftype == FOPCODE_SPARSE_SWITCH) {
    const uint16_t* tdata = data + 2 * entries;  // entries are 32b
    for (int i = 0; i < entries; i++) {
      int32_t index = read_int32(data);
      uint32_t targetaddr = base + read_int32(tdata);
      auto target = bm.by<Addr>().at(targetaddr);
      insert_multi_branch_target(fm, index, target, src);
    }
  } else {
    always_assert_log(false, "Bad fopcode 0x%04x in shard_multi_target", ftype);
  }
}

static void generate_branch_targets(
    FatMethod* fm,
    const EntryAddrBiMap& bm,
    const std::unordered_map<MethodItemEntry*, DexOpcodeData*>& entry_to_data) {
  for (auto miter = fm->begin(); miter != fm->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE) {
      auto insn = mentry->insn;
      if (is_branch(insn->opcode())) {
        if (is_multi_branch(insn->opcode())) {
          auto* fopcode_entry = get_target(mentry, bm);
          auto* fopcode = entry_to_data.at(fopcode_entry);
          shard_multi_target(fm, fopcode, mentry, bm);
          delete fopcode;
          // TODO: erase fopcode from map
        } else {
          auto target = get_target(mentry, bm);
          insert_branch_target(fm, target, mentry);
        }
      }
    }
  }
}

/*
 * Attach the pseudo opcodes representing fill-array-data-payload to the
 * fill-array-data instructions that reference them.
 */
void gather_array_data(
    FatMethod* fm,
    const EntryAddrBiMap& bm,
    const std::unordered_map<MethodItemEntry*, DexOpcodeData*>& entry_to_data) {
  for (MethodItemEntry& mie : InstructionIterable(fm)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      insn->set_data(entry_to_data.at(get_target(&mie, bm)));
    }
  }
}

static void associate_debug_entries(FatMethod* fm,
                                    DexDebugItem& dbg,
                                    const EntryAddrBiMap& bm) {
  for (auto& entry : dbg.get_entries()) {
    auto insert_point = bm.by<Addr>().at(entry.addr);
    MethodItemEntry* mentry;
    switch (entry.type) {
      case DexDebugEntryType::Instruction:
        mentry = new MethodItemEntry(std::move(entry.insn));
        break;
      case DexDebugEntryType::Position:
        mentry = new MethodItemEntry(std::move(entry.pos));
        break;
      default:
        not_reached();
    }
    fm->insert(fm->iterator_to(*insert_point), *mentry);
  }
  dbg.get_entries().clear();
}

static void associate_try_items(FatMethod* fm,
                                DexCode& code,
                                const EntryAddrBiMap& bm) {
  auto const& tries = code.get_tries();
  for (auto& tri : tries) {
    MethodItemEntry* catch_start = nullptr;
    CatchEntry* last_catch = nullptr;
    for (auto catz : tri->m_catches) {
      auto catzop = bm.by<Addr>().at(catz.second);
      TRACE(MTRANS, 3, "try_catch %08x mei %p\n", catz.second, catzop);
      auto catch_mei = new MethodItemEntry(catz.first);
      catch_start = catch_start == nullptr ? catch_mei : catch_start;
      if (last_catch != nullptr) {
        last_catch->next = catch_mei;
      }
      last_catch = catch_mei->centry;
      fm->insert(fm->iterator_to(*catzop), *catch_mei);
    }

    auto begin = bm.by<Addr>().at(tri->m_start_addr);
    TRACE(MTRANS, 3, "try_start %08x mei %p\n", tri->m_start_addr, begin);
    auto try_start = new MethodItemEntry(TRY_START, catch_start);
    fm->insert(fm->iterator_to(*begin), *try_start);
    uint32_t lastaddr = tri->m_start_addr + tri->m_insn_count;
    auto end = bm.by<Addr>().at(lastaddr);
    TRACE(MTRANS, 3, "try_end %08x mei %p\n", lastaddr, end);
    auto try_end = new MethodItemEntry(TRY_END, catch_start);
    fm->insert(fm->iterator_to(*end), *try_end);
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
    DexOpcode op;
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

void balloon(DexMethod* method, FatMethod* fmethod) {
  auto dex_code = method->get_dex_code();
  auto instructions = dex_code->release_instructions();
  // This is a 1-to-1 map between MethodItemEntries of type MFLOW_OPCODE and
  // address offsets.
  EntryAddrBiMap bm;
  std::unordered_map<MethodItemEntry*, DexOpcodeData*> entry_to_data;

  uint32_t addr = 0;
  for (auto insn : *instructions) {
    MethodItemEntry* mei;
    if (insn->opcode() == OPCODE_NOP || is_fopcode(insn->opcode())) {
      // We have to insert dummy entries for these opcodes so that try items
      // and debug entries that are adjacent to them can find the right
      // address.
      mei = new MethodItemEntry();
      if (is_fopcode(insn->opcode())) {
        entry_to_data.emplace(mei, static_cast<DexOpcodeData*>(insn));
      }
    } else {
      mei = new MethodItemEntry(new IRInstruction(insn));
    }
    fmethod->push_back(*mei);
    bm.insert(EntryAddrBiMap::relation(mei, addr));
    TRACE(MTRANS, 5, "%08x: %s[mei %p]\n", addr, SHOW(insn), mei);
    addr += insn->size();
  }
  bm.insert(EntryAddrBiMap::relation(&*fmethod->end(), addr));

  generate_branch_targets(fmethod, bm, entry_to_data);
  gather_array_data(fmethod, bm, entry_to_data);
  associate_try_items(fmethod, *dex_code, bm);
  auto debugitem = dex_code->get_debug_item();
  if (debugitem) {
    associate_debug_entries(fmethod, *debugitem, bm);
  }
}

/**
 * Map the `DexPositions` to a newly created clone. At the same time, it
 * preserves the relationship between a position and it's parent.
 */
std::unordered_map<DexPosition*, std::unique_ptr<DexPosition>>
get_old_to_new_position_copies(FatMethod* fmethod) {
  std::unordered_map<DexPosition*, std::unique_ptr<DexPosition>>
      old_position_to_new;
  for (auto& mie : *fmethod) {
    if (mie.type == MFLOW_POSITION) {
      old_position_to_new[mie.pos.get()] =
          std::make_unique<DexPosition>(*mie.pos);
    }
  }

  for (auto& old_to_new : old_position_to_new) {
    DexPosition* old_position = old_to_new.first;
    auto new_pos = old_to_new.second.get();

    new_pos->parent = old_position->parent
                          ? old_position_to_new[old_position->parent].get()
                          : nullptr;
  }

  return old_position_to_new;
}

// TODO: merge this and MethodSplicer.
FatMethod* deep_copy_fmethod(FatMethod* old_fmethod) {
  FatMethod* fmethod = new FatMethod();

  std::unordered_map<DexPosition*, std::unique_ptr<DexPosition>>
      old_position_to_new = get_old_to_new_position_copies(old_fmethod);

  // Create a clone for each of the entries.
  std::unordered_map<MethodItemEntry*, MethodItemEntry*> old_mentry_to_new;
  for (auto& mie : *old_fmethod) {

    // Since fallthorough entries are recomputed for the cfg, we can
    // skip those for now.
    if (mie.type == MFLOW_FALLTHROUGH) {
      continue;
    }

    MethodItemEntry* copy_item_entry = new MethodItemEntry();
    copy_item_entry->type = mie.type;

    switch (mie.type) {
      case MFLOW_TRY:
        copy_item_entry->tentry =
            new TryEntry(mie.tentry->type, mie.tentry->catch_start);
        break;
      case MFLOW_CATCH:
        copy_item_entry->centry = new CatchEntry(mie.centry->catch_type);
        break;
      case MFLOW_TARGET:
        copy_item_entry->target = new BranchTarget();
        copy_item_entry->target->type = mie.target->type;
        copy_item_entry->target->index = mie.target->index;
        break;
      case MFLOW_OPCODE:
        copy_item_entry->insn = new IRInstruction(*mie.insn);
        break;
      case MFLOW_DEBUG:
        new (&copy_item_entry->dbgop)
            std::unique_ptr<DexDebugInstruction>(mie.dbgop->clone());
        break;
      case MFLOW_POSITION:
        copy_item_entry->pos = std::move(old_position_to_new[mie.pos.get()]);
        break;
      default:
        not_reached();
    }

    old_mentry_to_new[&mie] = copy_item_entry;
  }

  // Preserve mapping between entries.
  for (auto& mie : *old_fmethod) {
    if (mie.type == MFLOW_FALLTHROUGH) {
      continue;
    }

    MethodItemEntry* copy_item_entry = old_mentry_to_new[&mie];
    switch (mie.type) {
      case MFLOW_TRY:
        copy_item_entry->tentry->catch_start =
           mie.tentry->catch_start ? old_mentry_to_new[mie.tentry->catch_start]
                                   : nullptr;
        break;
      case MFLOW_CATCH:
        copy_item_entry->centry->next =
          mie.centry->next ? old_mentry_to_new[mie.centry->next] : nullptr;
        break;
      case MFLOW_TARGET:
        copy_item_entry->target->src = old_mentry_to_new[mie.target->src];
        break;
      case MFLOW_OPCODE:
      case MFLOW_DEBUG:
      case MFLOW_POSITION:
        break;
      default:
        not_reached();
    }

    fmethod->push_back(*copy_item_entry);
  }

  return fmethod;
}

} // namespace

IRCode::IRCode(DexMethod* method): m_fmethod(new FatMethod()) {
  auto* dc = method->get_dex_code();
  generate_load_params(
      method, dc->get_registers_size() - dc->get_ins_size(), this);
  balloon(const_cast<DexMethod*>(method), m_fmethod);
  m_dbg = dc->release_debug_item();
}

IRCode::IRCode(DexMethod* method, size_t temp_regs)
    : m_fmethod(new FatMethod()) {
  always_assert(method->get_dex_code() == nullptr);
  generate_load_params(method, temp_regs, this);
}

IRCode::IRCode(const IRCode& code) {
  FatMethod* old_fmethod = code.m_fmethod;
  m_fmethod = deep_copy_fmethod(old_fmethod);
  m_registers_size = code.m_registers_size;
  if (code.m_dbg) {
    m_dbg = std::make_unique<DexDebugItem>(*code.m_dbg);
  }
}

void IRCode::remove_branch_target(IRInstruction *branch_inst) {
  always_assert_log(is_branch(branch_inst->opcode()),
                    "Instruction is not a branch instruction.");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_TARGET) {
      BranchTarget* bt = mentry->target;
      auto btmei = bt->src;
      if(btmei->insn == branch_inst) {
        mentry->type = MFLOW_FALLTHROUGH;
        delete mentry->target;
        mentry->throwing_mie = nullptr;
        break;
      }
    }
  }
}

void IRCode::replace_branch(IRInstruction* from, IRInstruction* to) {
  always_assert(is_branch(from->opcode()));
  always_assert(is_branch(to->opcode()));
  for (auto& mentry : *m_fmethod) {
    if (mentry.type == MFLOW_OPCODE && mentry.insn == from) {
      mentry.insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRCode::remove_debug_line_info(Block* block) {
  for (MethodItemEntry& mie : *block) {
    if (mie.type == MFLOW_POSITION) {
      mie.type = MFLOW_FALLTHROUGH;
      mie.pos.release();
    }
  }
}

void IRCode::replace_opcode_with_infinite_loop(IRInstruction* from) {
  IRInstruction* to = new IRInstruction(OPCODE_GOTO_32);
  to->set_offset(0);
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE && mentry->insn == from) {
      if (is_branch(from->opcode())) {
        remove_branch_target(from);
      }
      mentry->insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRCode::replace_opcode(IRInstruction* from, IRInstruction* to) {
  always_assert_log(!is_branch(to->opcode()),
                    "You may want replace_branch instead");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE && mentry->insn == from) {
      if (is_branch(from->opcode())) {
        remove_branch_target(from);
      }
      mentry->insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRCode::insert_after(IRInstruction* position,
                                   const std::vector<IRInstruction*>& opcodes) {
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
      for (auto* opcode : opcodes) {
        MethodItemEntry* mentry = new MethodItemEntry(opcode);
        m_fmethod->insert(insertat, *mentry);
      }
      return;
    }
  }
  always_assert_log(false, "No match found");
}

FatMethod::iterator IRCode::insert_before(
    const FatMethod::iterator& position, MethodItemEntry& mie) {
  return m_fmethod->insert(position, mie);
}

FatMethod::iterator IRCode::insert_after(
    const FatMethod::iterator& position, MethodItemEntry& mie) {
  always_assert(position != m_fmethod->end());
  return m_fmethod->insert(std::next(position), mie);
}

/*
 * Param `insn` should be part of a switch...case statement. Find the case
 * block it is contained within and remove it. Then decrement the index of
 * all the other case blocks that are larger than the index of the removed
 * block so that the case numbers don't have any gaps and the switch can
 * still be encoded as a packed-switch opcode.
 *
 * We do the removal by removing the MFLOW_TARGET corresponding to that
 * case label. Its contents are dead code which will be removed by LocalDCE
 * later. (We could do it here too, but LocalDCE already knows how to find
 * block boundaries.)
 */
void IRCode::remove_switch_case(IRInstruction* insn) {

  TRACE(MTRANS, 3, "Removing switch case from: %s\n", SHOW(m_fmethod));
  // Check if we are inside switch method.
  const MethodItemEntry* switch_mei {nullptr};
  for (const auto& mei : InstructionIterable(this)) {
    auto op = mei.insn->opcode();
    if (opcode::is_load_param(op)) {
      continue;
    }
    assert_log(is_multi_branch(op), " Method is not a switch");
    switch_mei = &mei;
    break;
  }
  always_assert(switch_mei != nullptr);

  int target_count = 0;
  for (auto& mei : *m_fmethod) {
    if (mei.type == MFLOW_TARGET && mei.target->type == BRANCH_MULTI) {
      target_count++;
    }
  }
  assert_log(target_count != 0, " There should be atleast one target");
  if (target_count == 1) {
    auto excpt_str = DexString::make_string("Redex switch Exception");
    std::vector<IRInstruction*> excpt_block;
    create_runtime_exception_block(excpt_str, excpt_block);
    insert_after(insn, excpt_block);
    remove_opcode(insn);
    return;
  }

  // Find the starting MULTI Target point to delete.
  MethodItemEntry* target_mei = nullptr;
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_TARGET) {
      target_mei = mentry;
    }
    // Check if insn belongs to the current block.
    if (mentry->type == MFLOW_OPCODE && mentry->insn == insn) {
      break;
    }
  }
  always_assert_log(target_mei != nullptr,
                    "Could not find target for %s in %s",
                    SHOW(insn),
                    SHOW(m_fmethod));

  for (const auto& mie : *m_fmethod) {
    if (mie.type == MFLOW_TARGET) {
      BranchTarget* bt = mie.target;
      if (bt->src == switch_mei && bt->index > target_mei->target->index) {
        bt->index -= 1;
      }
    }
  }

  target_mei->type = MFLOW_FALLTHROUGH;
  delete target_mei->target;
  target_mei->throwing_mie = nullptr;
}

size_t IRCode::count_opcodes() const {
  size_t count {0};
  for (const auto& mie : *m_fmethod) {
    if (mie.type == MFLOW_OPCODE &&
        !opcode::is_load_param(mie.insn->opcode())) {
      ++count;
    }
  }
  return count;
}

size_t IRCode::sum_opcode_sizes() const {
  size_t size {0};
  for (const auto& mie : *m_fmethod) {
    if (mie.type == MFLOW_OPCODE) {
      size += mie.insn->size();
    }
  }
  return size;
}

void IRCode::remove_opcode(const FatMethod::iterator& it) {
  always_assert(it->type == MFLOW_OPCODE);
  auto insn = it->insn;
  if (opcode::may_throw(insn->opcode())) {
    for (auto rev = --FatMethod::reverse_iterator(it);
         rev != m_fmethod->rend();
         ++rev) {
      if (rev->type == MFLOW_FALLTHROUGH && rev->throwing_mie) {
        assert(rev->throwing_mie == &*it);
        rev->throwing_mie = nullptr;
        break;
      } else if (rev->type == MFLOW_OPCODE) {
        break;
      }
    }
  }
  if (is_branch(insn->opcode())) {
    remove_branch_target(insn);
  }
  it->type = MFLOW_FALLTHROUGH;
  it->insn = nullptr;
  delete insn;
}

void IRCode::remove_opcode(IRInstruction* insn) {
  for (auto& mei : *m_fmethod) {
    if (mei.type == MFLOW_OPCODE && mei.insn == insn) {
      auto it = m_fmethod->iterator_to(mei);
      remove_opcode(it);
      return;
    }
  }
  always_assert_log(false,
                    "No match found while removing '%s' from method",
                    SHOW(insn));
}

FatMethod::iterator IRCode::main_block() {
  return std::prev(get_param_instructions().end());
}

FatMethod::iterator IRCode::insert(FatMethod::iterator cur,
                                   IRInstruction* insn) {
  MethodItemEntry* mentry = new MethodItemEntry(insn);
  return m_fmethod->insert(cur, *mentry);
}

FatMethod::iterator IRCode::make_if_block(
    FatMethod::iterator cur,
    IRInstruction* insn,
    FatMethod::iterator* false_block) {
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_fmethod->insert(cur, *if_entry);
  auto bt = new BranchTarget();
  bt->src = if_entry;
  bt->type = BRANCH_SIMPLE;
  auto bentry = new MethodItemEntry(bt);
  return m_fmethod->insert(m_fmethod->end(), *bentry);
}

FatMethod::iterator IRCode::make_if_else_block(
    FatMethod::iterator cur,
    IRInstruction* insn,
    FatMethod::iterator* false_block,
    FatMethod::iterator* true_block) {
  // if block
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_fmethod->insert(cur, *if_entry);

  // end of else goto
  auto goto_entry = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
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

FatMethod::iterator IRCode::make_switch_block(
    FatMethod::iterator cur,
    IRInstruction* insn,
    FatMethod::iterator* default_block,
    std::map<SwitchIndices, FatMethod::iterator>& cases) {
  auto switch_entry = new MethodItemEntry(insn);
  *default_block = m_fmethod->insert(cur, *switch_entry);
  FatMethod::iterator main_block = *default_block;
  for (auto case_it = cases.begin(); case_it != cases.end(); ++case_it) {
    auto goto_entry = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
    auto goto_it = m_fmethod->insert(m_fmethod->end(), *goto_entry);

    auto main_bt = new BranchTarget();
    main_bt->src = goto_entry;
    main_bt->type = BRANCH_SIMPLE;
    auto mb_entry = new MethodItemEntry(main_bt);
    main_block = m_fmethod->insert(++main_block, *mb_entry);

    // Insert all the branch targets jumping from the switch entry.
    // Keep updating the iterator of the case block to point right before the
    // GOTO going back to the end of the switch.
    for (auto idx : case_it->first) {
      auto case_bt = new BranchTarget();
      case_bt->src = switch_entry;
      case_bt->index = idx;
      case_bt->type = BRANCH_MULTI;
      auto eb_entry = new MethodItemEntry(case_bt);
      case_it->second = m_fmethod->insert(goto_it, *eb_entry);
    }
  }
  return main_block;
}

namespace {

using RegMap = transform::RegMap;

const char* DEBUG_ONLY show_reg_map(RegMap& map) {
  for (auto pair : map) {
    TRACE(INL, 5, "%u -> %u\n", pair.first, pair.second);
  }
  return "";
}

} // namespace

void IRCode::build_cfg(bool end_block_before_throw) {
  clear_cfg();
  m_cfg = std::make_unique<ControlFlowGraph>(this, end_block_before_throw);
}

void IRCode::clear_cfg() {
  m_cfg.reset();
  std::vector<FatMethod::iterator> fallthroughs;
  for (auto it = m_fmethod->begin(); it != m_fmethod->end(); ++it) {
    if (it->type == MFLOW_FALLTHROUGH) {
      fallthroughs.emplace_back(it);
    }
  }
  for (auto it : fallthroughs) {
    m_fmethod->erase_and_dispose(it, FatMethodDisposer());
  }
}

namespace ir_code_impl {

static uint16_t calc_outs_size(const IRCode* code) {
  uint16_t size {0};
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_invoke_range(insn->opcode())) {
      size = std::max(size, boost::numeric_cast<uint16_t>(insn->range_size()));
    } else if (is_invoke(insn->opcode())) {
      size = std::max(size, boost::numeric_cast<uint16_t>(insn->srcs_size()));
    }
  }
  return size;
}

/*
 * Check that load-param opcodes are all at the end of the register frame and
 * match the method proto, then remove those opcodes.
 *
 * Set the ins_size accordingly.
 */
static void sync_load_params(const DexMethod* method,
                             IRCode* code,
                             DexCode* dex_code) {
  auto param_ops = InstructionIterable(code->get_param_instructions());
  if (param_ops.empty()) {
    return;
  }
  auto& args_list = method->get_proto()->get_args()->get_type_list();
  auto it = param_ops.begin();
  auto end = param_ops.end();
  uint16_t ins_start = it->insn->dest();
  uint16_t next_ins = ins_start;
  if (!is_static(method)) {
    auto op = it->insn->opcode();
    always_assert(op == IOPCODE_LOAD_PARAM_OBJECT);
    it.reset(code->erase(it.unwrap()));
    ++next_ins;
  }
  auto args_it = args_list.begin();
  while (it != end) {
    auto op = it->insn->opcode();
    // check that the param registers are contiguous
    always_assert(next_ins == it->insn->dest());
    // TODO: have load param opcodes store the actual type of the param and
    // check that they match the method prototype here
    always_assert(args_it != args_list.end());
    if (is_wide_type(*args_it)) {
      always_assert(op == IOPCODE_LOAD_PARAM_WIDE);
    } else if (is_primitive(*args_it)) {
      always_assert(op == IOPCODE_LOAD_PARAM);
    } else {
      always_assert(op == IOPCODE_LOAD_PARAM_OBJECT);
    }
    ++args_it;
    next_ins += it->insn->dest_is_wide() ? 2 : 1;
    it.reset(code->erase(it.unwrap()));
  }
  always_assert(args_it == args_list.end());
  // check that the params are at the end of the frame
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->dests_size()) {
      always_assert(insn->dest() < next_ins);
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      always_assert(insn->src(i) < next_ins);
    }
  }
  dex_code->set_ins_size(next_ins - ins_start);
}

} // namespace ir_code_impl

using namespace ir_code_impl;

std::unique_ptr<DexCode> IRCode::sync(const DexMethod* method) {
  auto dex_code = std::make_unique<DexCode>();
  try {
    sync_load_params(method, this, &*dex_code);
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
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    TRACE(MTRANS, 5, "Analyzing mentry %p\n", mentry);
    entry_to_addr[mentry] = addr;
    if (mentry->type == MFLOW_OPCODE) {
      TRACE(MTRANS, 5, "Emitting mentry %p at %08x\n", mentry, addr);
      addr += mentry->insn->size();
    }
  }
  // Step 2, recalculate branches..., save off multi-branch data.
  TRACE(MTRANS, 5, "Recalculating branches\n");
  std::vector<MethodItemEntry*> multi_branches;
  std::unordered_map<MethodItemEntry*, std::vector<BranchTarget*>> multis;
  std::unordered_map<BranchTarget*, uint32_t> multi_targets;
  bool needs_resync = false;
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (entry_to_addr.find(mentry) == entry_to_addr.end()) {
      continue;
    }
    if (mentry->type == MFLOW_OPCODE) {
      auto opcode = mentry->insn->opcode();
      if (is_multi_branch(opcode)) {
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
      } else if (bt->type == BRANCH_SIMPLE) {
        MethodItemEntry* branch_op_mie = bt->src;
        int32_t branch_offset =
            entry_to_addr.at(mentry) - entry_to_addr.at(branch_op_mie);
        if (branch_offset == 1) {
          needs_resync = true;
          remove_opcode(branch_op_mie->insn);
        } else {
          needs_resync |=
              !encode_offset(m_fmethod, branch_op_mie, branch_offset);
        }
      }
    }
  }
  if (needs_resync) {
    return false;
  }
  auto& opout = code->reset_instructions();
  std::unordered_map<IRInstruction*, DexInstruction*> ir_to_dex_insn;
  for (auto& mie : InstructionIterable(this)) {
    TRACE(MTRANS, 6, "Emitting insn %s\n", SHOW(mie.insn));
    auto dex_insn = mie.insn->to_dex_instruction();
    opout.push_back(dex_insn);
    ir_to_dex_insn.emplace(mie.insn, dex_insn);
  }
  TRACE(MTRANS, 5, "Emitting multi-branches\n");
  // Step 3, generate multi-branch fopcodes
  for (auto multiopcode : multi_branches) {
    auto& targets = multis[multiopcode];
    auto multi_insn = ir_to_dex_insn.at(multiopcode->insn);
    std::sort(targets.begin(), targets.end(), multi_target_compare_index);
    always_assert_log(!targets.empty(), "need to have targets");
    if (multi_contains_gaps(targets)) {
      // Emit sparse.
      unsigned long count = (targets.size() * 4) + 2;
      uint16_t sparse_payload[count];
      sparse_payload[0] = FOPCODE_SPARSE_SWITCH;
      sparse_payload[1] = targets.size();
      uint32_t* spkeys = (uint32_t*)&sparse_payload[2];
      uint32_t* sptargets =
          (uint32_t*)&sparse_payload[2 + (targets.size() * 2)];
      for (BranchTarget* target : targets) {
        *spkeys++ = target->index;
        *sptargets++ = multi_targets[target] - entry_to_addr.at(multiopcode);
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(0);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop = new DexOpcodeData(sparse_payload, (int)(count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      multi_insn->set_offset(addr - entry_to_addr.at(multiopcode));
      multi_insn->set_opcode(OPCODE_SPARSE_SWITCH);
      addr += count;
    } else {
      // Emit packed.
      unsigned long count = (targets.size() * 2) + 4;
      uint16_t packed_payload[count];
      packed_payload[0] = FOPCODE_PACKED_SWITCH;
      packed_payload[1] = targets.size();
      uint32_t* psdata = (uint32_t*)&packed_payload[2];
      *psdata++ = targets.front()->index;
      for (BranchTarget* target : targets) {
        *psdata++ = multi_targets[target] - entry_to_addr.at(multiopcode);
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
      multi_insn->set_offset(addr - entry_to_addr.at(multiopcode));
      multi_insn->set_opcode(OPCODE_PACKED_SWITCH);
      addr += count;
    }
  }

  TRACE(MTRANS, 5, "Emitting filled array data\n");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = ir_to_dex_insn.at(mentry->insn);
    if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      if (addr & 1) {
        opout.push_back(new DexInstruction(OPCODE_NOP));
        ++addr;
      }
      insn->set_offset(addr - entry_to_addr.at(mentry));
      auto fopcode = mentry->insn->get_data();
      opout.push_back(fopcode);
      addr += fopcode->size();
    }
  }

  // Step 4, emit debug opcodes
  TRACE(MTRANS, 5, "Emitting debug opcodes\n");
  auto debugitem = code->get_debug_item();
  if (debugitem) {
    auto& entries = debugitem->get_entries();
    for (auto& mentry : *m_fmethod) {
      if (mentry.type == MFLOW_DEBUG) {
        entries.emplace_back(entry_to_addr.at(&mentry), std::move(mentry.dbgop));
      } else if (mentry.type == MFLOW_POSITION) {
        entries.emplace_back(entry_to_addr.at(&mentry), std::move(mentry.pos));
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

    always_assert(try_end->tentry->catch_start ==
                  try_start->tentry->catch_start);
    auto insn_count = entry_to_addr.at(try_end) - entry_to_addr.at(try_start);
    if (insn_count == 0) {
      continue;
    }
    auto try_item = new DexTryItem(entry_to_addr.at(try_start), insn_count);
    for (auto mei = try_end->tentry->catch_start;
        mei != nullptr;
        mei = mei->centry->next) {
      try_item->m_catches.emplace_back(mei->centry->catch_type,
                                       entry_to_addr.at(mei));
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

bool InstructionIterable::structural_equals(const InstructionIterable& other) {
  auto it1 = this->begin();
  auto it2 = other.begin();

  for (; it1 != this->end() && it2 != other.end(); it1++, it2++) {
    auto& mie1 = *it1;
    auto& mie2 = *it2;

    if (*mie1.insn != *mie2.insn) {
      return false;
    }
  }

  return it1 == this->end() && it2 == other.end();
}
