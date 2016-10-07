/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexClass.h"

#include <algorithm>
#include <boost/functional/hash.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Debug.h"
#include "DexDefs.h"
#include "DexAccess.h"
#include "DexDebugInstruction.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Util.h"
#include "Warning.h"

int DexTypeList::encode(DexOutputIdx* dodx, uint32_t* output) {
  uint16_t* typep = (uint16_t*)(output + 1);
  *output = (uint32_t) m_list.size();
  for (auto const& type : m_list) {
    *typep++ = dodx->typeidx(type);
  }
  return (int) (((uint8_t*)typep) - (uint8_t*)output);
}

void DexField::make_concrete(DexAccessFlags access_flags, DexEncodedValue* v) {
  // FIXME assert if already concrete
  m_value = v;
  m_access = access_flags;
  m_concrete = true;
}

DexDebugEntry::DexDebugEntry(DexDebugEntry&& that)
    : type(that.type), addr(that.addr) {
  switch (type) {
  case DexDebugEntryType::Position:
    new (&pos) std::unique_ptr<DexPosition>(std::move(that.pos));
    break;
  case DexDebugEntryType::Instruction:
    new (&insn) std::unique_ptr<DexDebugInstruction>(std::move(that.insn));
    break;
  }
}

DexDebugEntry::~DexDebugEntry() {
  switch (type) {
  case DexDebugEntryType::Position:
    pos.~unique_ptr<DexPosition>();
    break;
  case DexDebugEntryType::Instruction:
    insn.~unique_ptr<DexDebugInstruction>();
    break;
  }
}

/*
 * Evaluate the debug opcodes to figure out their absolute addresses and line
 * numbers.
 */
static std::vector<DexDebugEntry> eval_debug_instructions(
    DexDebugItem* dbg,
    std::vector<std::unique_ptr<DexDebugInstruction>>& insns,
    DexString* source_file
    ) {
  std::vector<DexDebugEntry> entries;
  int32_t absolute_line = int32_t(dbg->get_line_start());
  uint32_t pc = 0;
  for (auto& opcode : insns) {
    auto op = opcode->opcode();
    switch (op) {
    case DBG_ADVANCE_LINE: {
      absolute_line += opcode->value();
      continue;
    }
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL:
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED:
    case DBG_SET_FILE:
    case DBG_END_SEQUENCE:
    case DBG_SET_PROLOGUE_END:
    case DBG_SET_EPILOGUE_BEGIN: {
      entries.emplace_back(pc, std::move(opcode));
      break;
    }
    case DBG_ADVANCE_PC: {
      pc += opcode->uvalue();
      continue;
    }
    default: {
      uint8_t adjustment = op - DBG_FIRST_SPECIAL;
      absolute_line += DBG_LINE_BASE + (adjustment % DBG_LINE_RANGE);
      pc += adjustment / DBG_LINE_RANGE;
      if (source_file != nullptr) {
        entries.emplace_back(
            pc, std::make_unique<DexPosition>(source_file, absolute_line));
      }
      break;
    }
    }
  }
  return entries;
}

DexDebugItem::DexDebugItem(DexIdx* idx, uint32_t offset,
    DexString* source_file) {
  const uint8_t* encdata = idx->get_uleb_data(offset);
  m_line_start = read_uleb128(&encdata);
  uint32_t paramcount = read_uleb128(&encdata);
  while (paramcount--) {
    DexString* str = decode_noindexable_string(idx, encdata);
    m_param_names.push_back(str);
  }
  std::vector<std::unique_ptr<DexDebugInstruction>> insns;
  DexDebugInstruction* dbgp;
  while ((dbgp = DexDebugInstruction::make_instruction(idx, encdata)) != nullptr) {
    insns.emplace_back(dbgp);
  }
  m_dbg_entries = eval_debug_instructions(this, insns, source_file);
}

DexDebugItem::DexDebugItem(const DexDebugItem& that)
    : m_line_start(that.m_line_start), m_param_names(that.m_param_names) {
  std::unordered_map<DexPosition*, DexPosition*> pos_map;
  for (auto& entry : that.m_dbg_entries) {
    switch (entry.type) {
    case DexDebugEntryType::Position: {
      auto pos = std::make_unique<DexPosition>(*entry.pos);
      pos_map[entry.pos.get()] = pos.get();
      pos->parent = pos_map[pos->parent];
      m_dbg_entries.emplace_back(entry.addr, std::move(pos));
      break;
    }
    case DexDebugEntryType::Instruction:
      m_dbg_entries.emplace_back(entry.addr, entry.insn->clone());
      break;
    }
  }
}

std::unique_ptr<DexDebugItem> DexDebugItem::get_dex_debug(
    DexIdx* idx, uint32_t offset, DexString* source_file) {
  if (offset == 0) return nullptr;
  return std::unique_ptr<DexDebugItem>(
      new DexDebugItem(idx, offset, source_file));
}

namespace {

/*
 * Convert DexDebugEntries into debug opcodes.
 */
std::vector<std::unique_ptr<DexDebugInstruction>> generate_debug_instructions(
    DexDebugItem* debugitem,
    DexOutputIdx* dodx,
    PositionMapper* pos_mapper,
    uint8_t* output) {
  std::vector<std::unique_ptr<DexDebugInstruction>> dbgops;
  uint32_t prev_addr = 0;
  uint32_t prev_line = pos_mapper->get_next_line(debugitem);
  auto& entries = debugitem->get_entries();

  for (auto it = entries.begin(); it != entries.end(); ++it) {
    // find all entries that belong to the same address, and group them by type
    auto addr = it->addr;
    std::vector<DexPosition*> positions;
    std::vector<DexDebugInstruction*> insns;
    for (; it != entries.end() && it->addr == addr; ++it) {
      switch (it->type) {
        case DexDebugEntryType::Position:
          positions.push_back(it->pos.get());
          break;
        case DexDebugEntryType::Instruction:
          insns.push_back(it->insn.get());
          break;
      }
    }
    --it;
    auto addr_delta = addr - prev_addr;
    prev_addr = addr;

    for (auto pos : positions) {
      pos_mapper->register_position(pos);
    }
    // only emit the last position entry for a given address
    if (!positions.empty()) {
      auto line = pos_mapper->position_to_line(positions.back());
      int32_t line_delta = line - prev_line;
      prev_line = line;
      if (line_delta < DBG_LINE_BASE ||
              line_delta >= (DBG_LINE_RANGE + DBG_LINE_BASE)) {
        dbgops.emplace_back(new DexDebugInstruction(
              DBG_ADVANCE_LINE, line_delta));
        line_delta = 0;
      }
      auto special = (line_delta - DBG_LINE_BASE) +
                       (addr_delta * DBG_LINE_RANGE) + DBG_FIRST_SPECIAL;
      if (special & ~0xff) {
        dbgops.emplace_back(new DexDebugInstruction(
              DBG_ADVANCE_PC, uint32_t(addr_delta)));
        special = line_delta - DBG_LINE_BASE + DBG_FIRST_SPECIAL;
      }
      dbgops.emplace_back(new DexDebugInstruction(
            static_cast<DexDebugItemOpcode>(special)));
      line_delta = 0;
      addr_delta = 0;
    }

    for (auto insn : insns) {
      if (addr_delta != 0) {
        dbgops.emplace_back(new DexDebugInstruction(
              DBG_ADVANCE_PC, addr_delta));
        addr_delta = 0;
      }
      dbgops.emplace_back(insn->clone());
    }
  }
  return dbgops;
}

}

int DexDebugItem::encode(DexOutputIdx* dodx, PositionMapper* pos_mapper,
    uint8_t* output) {
  uint8_t* encdata = output;
  encdata = write_uleb128(encdata, pos_mapper->get_next_line(this));
  encdata = write_uleb128(encdata, (uint32_t) m_param_names.size());
  for (auto s : m_param_names) {
    if (s == nullptr) {
      encdata = write_uleb128p1(encdata, DEX_NO_INDEX);
      continue;
    }
    uint32_t idx = dodx->stringidx(s);
    encdata = write_uleb128p1(encdata, idx);
  }
  auto dbgops = generate_debug_instructions(this, dodx, pos_mapper, encdata);
  for (auto& dbgop : dbgops) {
    dbgop->encode(dodx, encdata);
  }
  encdata = write_uleb128(encdata, DBG_END_SEQUENCE);
  return (int) (encdata - output);
}

void DexDebugItem::gather_types(std::vector<DexType*>& ltype) {
  for (auto& entry : m_dbg_entries) {
    entry.gather_types(ltype);
  }
}

void DexDebugItem::gather_strings(std::vector<DexString*>& lstring) {
  for (auto p : m_param_names) {
    if (p) lstring.push_back(p);
  }
  for (auto& entry : m_dbg_entries) {
    entry.gather_strings(lstring);
  }
}

DexCode::DexCode(const DexCode& that)
    : m_registers_size(that.m_registers_size),
      m_ins_size(that.m_ins_size),
      m_outs_size(that.m_outs_size),
      m_insns(std::make_unique<std::vector<DexInstruction*>>()) {
  for (auto& insn : *that.m_insns) {
    m_insns->emplace_back(insn->clone());
  }
  for (auto& try_ : that.m_tries) {
    m_tries.emplace_back(new DexTryItem(*try_));
  }
  if (that.m_dbg) {
    m_dbg.reset(new DexDebugItem(*that.m_dbg));
  }
}

std::unique_ptr<DexCode> DexCode::get_dex_code(DexIdx* idx,
                                               uint32_t offset,
                                               DexString* source_file) {
  if (offset == 0) return std::unique_ptr<DexCode>();
  const dex_code_item* code = (const dex_code_item*)idx->get_uint_data(offset);
  std::unique_ptr<DexCode> dc(new DexCode());
  dc->m_registers_size = code->registers_size;
  dc->m_ins_size = code->ins_size;
  dc->m_outs_size = code->outs_size;
  dc->m_insns.reset(new std::vector<DexInstruction*>());
  const uint16_t* cdata = (const uint16_t*)(code + 1);
  uint32_t tries = code->tries_size;
  if (code->insns_size) {
    const uint16_t* end = cdata + code->insns_size;
    while (cdata < end) {
      DexInstruction* dop = DexInstruction::make_instruction(idx, cdata);
      always_assert_log(
          dop != nullptr, "Failed to parse method at offset 0x%08x", offset);
      dc->m_insns->push_back(dop);
    }
    /*
     * Padding, see dex-spec.
     * Per my memory, there are dex-files where the padding is
     * implemented not according to spec.  Just FYI in case
     * something weird happens in the future.
     */
    if (code->insns_size & 1 && tries) cdata++;
  }

  if (tries) {
    const dex_tries_item* dti = (const dex_tries_item*)cdata;
    const uint8_t* handlers = (const uint8_t*)(dti + tries);
    for (uint32_t i = 0; i < tries; i++) {
      DexTryItem* dextry = new DexTryItem(dti[i].start_addr, dti[i].insn_count);
      const uint8_t* handler = handlers + dti[i].handler_off;
      int32_t count = read_sleb128(&handler);
      bool has_catchall = false;
      if (count <= 0) {
        count = -count;
        has_catchall = true;
      }
      while (count--) {
        uint32_t tidx = read_uleb128(&handler);
        uint32_t hoff = read_uleb128(&handler);
        DexType* dt = idx->get_typeidx(tidx);
        dextry->m_catches.push_back(std::make_pair(dt, hoff));
      }
      if (has_catchall) {
        auto hoff = read_uleb128(&handler);
        dextry->m_catches.push_back(std::make_pair(nullptr, hoff));
      }
      dc->m_tries.emplace_back(dextry);
    }
  }
  dc->m_dbg = DexDebugItem::get_dex_debug(idx, code->debug_info_off,
      source_file);
  return dc;
}

int DexCode::encode(DexOutputIdx* dodx, uint32_t* output) {
  dex_code_item* code = (dex_code_item*)output;
  code->registers_size = m_registers_size;
  code->ins_size = m_ins_size;
  code->outs_size = m_outs_size;
  code->tries_size = 0;
  /* Debug info is added later */
  code->debug_info_off = 0;
  uint16_t* insns = (uint16_t*)(code + 1);
  for (auto const& opc : get_instructions()) {
    opc->encode(dodx, insns);
  }
  code->insns_size = (uint32_t) (insns - ((uint16_t*)(code + 1)));
  if (m_tries.size() == 0)
    return ((code->insns_size * sizeof(uint16_t)) + sizeof(dex_code_item));
  /*
   * Now the tries..., obscenely messy encoding :(
   * Pad tries to uint32_t
   */
  if (code->insns_size & 1) insns++;
  int tries = code->tries_size = m_tries.size();
  dex_tries_item* dti = (dex_tries_item*)insns;
  uint8_t* handler_base = (uint8_t*)(dti + tries);
  uint8_t* hemit = handler_base;
  std::unordered_set<DexCatches, boost::hash<DexCatches>> catches_set;
  for (auto& dextry : m_tries) {
    catches_set.insert(dextry->m_catches);
  }
  hemit = write_uleb128(hemit, catches_set.size());
  int tryno = 0;
  std::unordered_map<DexCatches, uint32_t, boost::hash<DexCatches>> catches_map;
  for (auto it = m_tries.begin(); it != m_tries.end(); ++it, ++tryno) {
    auto& dextry = *it;
    dti[tryno].start_addr = dextry->m_start_addr;
    dti[tryno].insn_count = dextry->m_insn_count;
    if (catches_map.find(dextry->m_catches) == catches_map.end()) {
      catches_map[dextry->m_catches] = hemit - handler_base;
      size_t catchcount = dextry->m_catches.size();
      bool has_catchall =
        dextry->m_catches.back().first == nullptr;
      if (has_catchall) {
        catchcount = -(catchcount - 1);
      }
      hemit = write_sleb128(hemit, (int32_t) catchcount);
      for (auto const& cit : dextry->m_catches) {
        auto type = cit.first;
        if (type != nullptr) {
          hemit = write_uleb128(hemit, dodx->typeidx(type));
        }
        hemit = write_uleb128(hemit, cit.second);
      }
    }
    dti[tryno].handler_off = catches_map.at(dextry->m_catches);
  }
  return (int) (hemit - ((uint8_t*)output));
}

DexMethod* DexMethod::make_method_from(DexMethod* that,
                                       DexType* target_cls,
                                       DexString* name) {
  auto m = DexMethod::make_method(target_cls, name, that->get_proto());
  assert(m != that);
  if (that->m_anno) {
    m->m_anno = new DexAnnotationSet(*that->m_anno);
  }
  m->m_code.reset(new DexCode(*that->m_code));
  m->m_access = that->m_access;
  m->m_concrete = that->m_concrete;
  m->m_virtual = that->m_virtual;
  m->m_external = that->m_external;
  for (auto& pair : that->m_param_anno) {
    // note: DexAnnotation's copy ctor only does a shallow copy
    m->m_param_anno.emplace(pair.first, new DexAnnotationSet(*pair.second));
  }
  return m;
}

namespace {
std::vector<std::string> split_args(std::string args) {
  std::vector<std::string> ret;
  auto begin = size_t{0};
  while (begin < args.length()) {
    auto ch = args[begin];
    auto end = begin + 1;
    if (ch == '[') {
      while (args[end] == '[') {
        ++end;
      }
      ch = args[end];
      ++end;
    }
    if (ch == 'L') {
      auto semipos = args.find(';', end);
      assert(semipos != std::string::npos);
      end = semipos + 1;
    }
    ret.emplace_back(args.substr(begin, end - begin));
    begin = end;
  }
  return ret;
}
}

DexMethod* DexMethod::get_method(std::string canon) {
  auto cls_end = canon.find('.');
  auto name_start = cls_end + 1;
  auto name_end = canon.find('(', name_start);
  auto args_start = name_end + 1;
  auto args_end = canon.find(')', args_start);
  auto rtype_start = args_end + 1;
  auto cls_str = canon.substr(0, cls_end);
  auto name_str = canon.substr(name_start, name_end - name_start);
  auto args_str = canon.substr(args_start, args_end - args_start);
  auto rtype_str = canon.substr(rtype_start);
  std::list<DexType*> args;
  for (auto const& arg_str : split_args(args_str)) {
    args.push_back(DexType::get_type(arg_str.c_str()));
  }
  auto dtl = DexTypeList::get_type_list(std::move(args));
  return get_method(
    DexType::get_type(cls_str.c_str()),
    DexString::get_string(name_str.c_str()),
    DexProto::get_proto(DexType::get_type(rtype_str.c_str()), dtl));
}

void DexMethod::become_virtual() {
  assert(!m_virtual);
  m_virtual = true;
  auto cls = type_class(m_ref.cls);
  assert(!cls->is_external());
  auto& dmethods = cls->get_dmethods();
  auto& vmethods = cls->get_vmethods();
  dmethods.remove(this);
  insert_sorted(vmethods, this, compare_dexmethods);
}

void DexMethod::make_concrete(DexAccessFlags access,
                              std::unique_ptr<DexCode> dc,
                              bool is_virtual) {
  m_access = access;
  m_code = std::move(dc);
  m_concrete = true;
  m_virtual = is_virtual;
}

/*
 * See class_data_item in Dex spec.
 */
void DexClass::load_class_data_item(DexIdx* idx,
                                    uint32_t cdi_off,
                                    DexEncodedValueArray* svalues) {
  if (cdi_off == 0) return;
  m_has_class_data = true;
  const uint8_t* encd = idx->get_uleb_data(cdi_off);
  uint32_t sfield_count = read_uleb128(&encd);
  uint32_t ifield_count = read_uleb128(&encd);
  uint32_t dmethod_count = read_uleb128(&encd);
  uint32_t vmethod_count = read_uleb128(&encd);
  uint32_t ndex = 0;
  for (uint32_t i = 0; i < sfield_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    DexField* df = idx->get_fieldidx(ndex);
    DexEncodedValue* ev = nullptr;
    if (svalues != nullptr) {
      ev = svalues->pop_next();
    }
    df->make_concrete(access_flags, ev);
    m_sfields.push_back(df);
  }
  ndex = 0;
  for (uint32_t i = 0; i < ifield_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    DexField* df = idx->get_fieldidx(ndex);
    df->make_concrete(access_flags);
    m_ifields.push_back(df);
  }
  ndex = 0;
  for (uint32_t i = 0; i < dmethod_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    uint32_t code_off = read_uleb128(&encd);
    DexMethod* dm = idx->get_methodidx(ndex);
    std::unique_ptr<DexCode> dc =
        DexCode::get_dex_code(idx, code_off, m_source_file);
    dm->make_concrete(access_flags, std::move(dc), false);
    m_dmethods.push_back(dm);
  }
  ndex = 0;
  for (uint32_t i = 0; i < vmethod_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    uint32_t code_off = read_uleb128(&encd);
    DexMethod* dm = idx->get_methodidx(ndex);
    std::unique_ptr<DexCode> dc =
        DexCode::get_dex_code(idx, code_off, m_source_file);
    dm->make_concrete(access_flags, std::move(dc), true);
    m_vmethods.push_back(dm);
  }
}

void DexClass::add_method(DexMethod* m) {
  always_assert_log(m->is_concrete() || m->is_external(),
                    "Method %s must be concrete",
                    SHOW(m));
  always_assert(m->get_class() == get_type());
  if (m->is_virtual()) {
    insert_sorted(m_vmethods, m, compare_dexmethods);
  } else {
    insert_sorted(m_dmethods, m, compare_dexmethods);
  }
}

void DexClass::remove_method(DexMethod* m) {
  auto& meths = m->is_virtual() ? m_vmethods : m_dmethods;
  DEBUG_ONLY bool erased = false;
  for (auto it = meths.begin(); it != meths.end(); it++) {
    if (*it == m) {
      erased = true;
      meths.erase(it);
      break;
    }
  }
  assert(erased);
}

void DexClass::add_field(DexField* f) {
  always_assert_log(f->is_concrete() || f->is_external(),
                    "Field %s must be concrete",
                    SHOW(f));
  always_assert(f->get_class() == get_type());
  bool is_static = f->get_access() & DexAccessFlags::ACC_STATIC;
  if (is_static) {
    insert_sorted(m_sfields, f, compare_dexfields);
  } else {
    insert_sorted(m_ifields, f, compare_dexfields);
  }
}

void DexClass::remove_field(DexField* f) {
  bool is_static = f->get_access() & DexAccessFlags::ACC_STATIC;
  auto& fields = is_static ? m_sfields : m_ifields;
  DEBUG_ONLY bool erase = false;
  for (auto it = fields.begin(); it != fields.end(); it++) {
    if (*it == f) {
      erase = true;
      it = fields.erase(it);
      break;
    }
  }
  assert(erase);
}

int DexClass::encode(DexOutputIdx* dodx,
                     dexcode_to_offset& dco,
                     uint8_t* output) {
  if (m_sfields.size() == 0 && m_ifields.size() == 0 &&
      m_dmethods.size() == 0 && m_vmethods.size() == 0) {
    opt_warn(PURE_ABSTRACT_CLASS,
             "'%s' super '%s' flags 0x%08x\n",
             m_self->get_name()->c_str(),
             m_super_class->get_name()->c_str(),
             m_access_flags);
  }
  uint8_t* encdata = output;
  encdata = write_uleb128(encdata, (uint32_t) m_sfields.size());
  encdata = write_uleb128(encdata, (uint32_t) m_ifields.size());
  encdata = write_uleb128(encdata, (uint32_t) m_dmethods.size());
  encdata = write_uleb128(encdata, (uint32_t) m_vmethods.size());
  uint32_t idxbase;
  idxbase = 0;
  for (auto const& f : m_sfields) {
    uint32_t idx = dodx->fieldidx(f);
    always_assert_log(idx >= idxbase,
                      "Illegal ordering for sfield, need to apply sort. "
                      "Must be done prior to static value emit."
                      "\nOffending type: %s"
                      "\nOffending field: %s",
                      SHOW(this),
                      SHOW(f));
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, f->get_access());
  }
  idxbase = 0;
  for (auto const& f : m_ifields) {
    uint32_t idx = dodx->fieldidx(f);
    always_assert_log(idx >= idxbase,
                      "Illegal ordering for ifield, need to apply sort."
                      "\nOffending type: %s"
                      "\nOffending field: %s",
                      SHOW(this),
                      SHOW(f));
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, f->get_access());
  }
  idxbase = 0;
  for (auto const& m : m_dmethods) {
    uint32_t idx = dodx->methodidx(m);
    always_assert_log(!m->is_virtual(),
                      "Virtual method in dmethod."
                      "\nOffending type: %s"
                      "\nOffending method: %s",
                      SHOW(this),
                      SHOW(m));
    assert(!m->is_virtual());
    always_assert_log(idx >= idxbase,
                      "Illegal ordering for dmethod, need to apply sort."
                      "\nOffending type: %s"
                      "\nOffending method: %s",
                      SHOW(this),
                      SHOW(m));
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, m->get_access());
    uint32_t code_off = 0;
    if (m->get_code() != nullptr && dco.count(m->get_code().get())) {
      code_off = dco[m->get_code().get()];
    }
    encdata = write_uleb128(encdata, code_off);
  }
  idxbase = 0;
  for (auto const& m : m_vmethods) {
    uint32_t idx = dodx->methodidx(m);
    always_assert_log(m->is_virtual(),
                      "Direct method in vmethod."
                      "\nOffending type: %s"
                      "\nOffending method: %s",
                      SHOW(this),
                      SHOW(m));
    assert(m->is_virtual());
    always_assert_log(idx >= idxbase,
                      "Illegal ordering for vmethod, need to apply sort."
                      "\nOffending type: %s"
                      "\nOffending method: %s",
                      SHOW(this),
                      SHOW(m));
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, m->get_access());
    uint32_t code_off = 0;
    if (m->get_code() != nullptr && dco.count(m->get_code().get())) {
      code_off = dco[m->get_code().get()];
    }
    encdata = write_uleb128(encdata, code_off);
  }
  return (int) (encdata - output);
}

void DexClass::load_class_annotations(DexIdx* idx, uint32_t anno_off) {
  if (anno_off == 0) return;
  const dex_annotations_directory_item* annodir =
    (const dex_annotations_directory_item*)idx->get_uint_data(anno_off);
  m_anno =
      DexAnnotationSet::get_annotation_set(idx, annodir->class_annotations_off);
  const uint32_t* annodata = (uint32_t*)(annodir + 1);
  for (uint32_t i = 0; i < annodir->fields_size; i++) {
    uint32_t fidx = *annodata++;
    uint32_t off = *annodata++;
    DexField* field = idx->get_fieldidx(fidx);
    DexAnnotationSet* aset = DexAnnotationSet::get_annotation_set(idx, off);
    field->attach_annotation_set(aset);
  }
  for (uint32_t i = 0; i < annodir->methods_size; i++) {
    uint32_t midx = *annodata++;
    uint32_t off = *annodata++;
    DexMethod* method = idx->get_methodidx(midx);
    DexAnnotationSet* aset = DexAnnotationSet::get_annotation_set(idx, off);
    method->attach_annotation_set(aset);
  }
  for (uint32_t i = 0; i < annodir->parameters_size; i++) {
    uint32_t midx = *annodata++;
    uint32_t xrefoff = *annodata++;
    if (xrefoff != 0) {
      DexMethod* method = idx->get_methodidx(midx);
      const uint32_t* annoxref = idx->get_uint_data(xrefoff);
      uint32_t count = *annoxref++;
      for (uint32_t j = 0; j < count; j++) {
        uint32_t off = annoxref[j];
        DexAnnotationSet* aset = DexAnnotationSet::get_annotation_set(idx, off);
        if (aset != nullptr) {
          method->attach_param_annotation_set(j, aset);
          assert(method->get_param_anno());
        }
      }
    }
  }
}

static DexEncodedValueArray* load_static_values(DexIdx* idx, uint32_t sv_off) {
  if (sv_off == 0) return nullptr;
  const uint8_t* encd = idx->get_uleb_data(sv_off);
  return get_encoded_value_array(idx, encd);
}

DexEncodedValueArray* DexClass::get_static_values() {
  bool has_static_values = false;
  for (auto const& f : m_sfields) {
    if (f->get_static_value() != nullptr) {
      has_static_values = true;
      break;
    }
  }
  if (!has_static_values) return nullptr;

  std::list<DexEncodedValue*>* aev = new std::list<DexEncodedValue*>();
  for (auto const& f : m_sfields) {
    DexEncodedValue* ev = f->get_static_value();
    if (ev == nullptr) {
      has_static_values = false;
      continue;
    }
    always_assert_log(has_static_values, "Hole in static value ordering");
    aev->push_back(ev);
  }
  return new DexEncodedValueArray(aev, true);
}

DexAnnotationDirectory* DexClass::get_annotation_directory() {
  /* First scan to see what types of annotations to scan for if any.
   */
  DexFieldAnnotations* fanno = nullptr;
  DexMethodAnnotations* manno = nullptr;
  DexMethodParamAnnotations* mpanno = nullptr;

  for (auto const& f : m_sfields) {
    if (f->get_anno_set()) {
      if (fanno == nullptr) {
        fanno = new DexFieldAnnotations();
      }
      fanno->push_back(std::make_pair(f, f->get_anno_set()));
    }
  }
  for (auto const& f : m_ifields) {
    if (f->get_anno_set()) {
      if (fanno == nullptr) {
        fanno = new DexFieldAnnotations();
      }
      fanno->push_back(std::make_pair(f, f->get_anno_set()));
    }
  }
  for (auto const& m : m_dmethods) {
    if (m->get_anno_set()) {
      if (manno == nullptr) {
        manno = new DexMethodAnnotations();
      }
      manno->push_back(std::make_pair(m, m->get_anno_set()));
    }
    if (m->get_param_anno()) {
      if (mpanno == nullptr) {
        mpanno = new DexMethodParamAnnotations();
      }
      mpanno->push_back(std::make_pair(m, m->get_param_anno()));
    }
  }
  for (auto const& m : m_vmethods) {
    if (m->get_anno_set()) {
      if (manno == nullptr) {
        manno = new DexMethodAnnotations();
      }
      manno->push_back(std::make_pair(m, m->get_anno_set()));
    }
    if (m->get_param_anno()) {
      if (mpanno == nullptr) {
        mpanno = new DexMethodParamAnnotations();
      }
      mpanno->push_back(std::make_pair(m, m->get_param_anno()));
    }
  }
  if (m_anno || fanno || manno || mpanno) {
    return new DexAnnotationDirectory(m_anno, fanno, manno, mpanno);
  }
  return nullptr;
}

DexClass::DexClass(DexIdx* idx, dex_class_def* cdef) {
  m_anno = nullptr;
  m_has_class_data = false;
  m_external = false;
  m_self = idx->get_typeidx(cdef->typeidx);
  m_access_flags = (DexAccessFlags)cdef->access_flags;
  m_super_class = idx->get_typeidx(cdef->super_idx);
  m_interfaces = idx->get_type_list(cdef->interfaces_off);
  m_source_file = idx->get_nullable_stringidx(cdef->source_file_idx);
  load_class_annotations(idx, cdef->annotations_off);
  DexEncodedValueArray* deva = load_static_values(idx, cdef->static_values_off);
  load_class_data_item(idx, cdef->class_data_offset, deva);
  g_redex->build_type_system(this);
  delete (deva);
}

void DexTypeList::gather_types(std::vector<DexType*>& ltype) {
  for (auto const& type : m_list) {
    ltype.push_back(type);
  }
}

static DexString* make_shorty(DexType* rtype, DexTypeList* args) {
  std::stringstream ss;
  ss << type_shorty(rtype);
  if (args != nullptr) {
    for (auto arg : args->get_type_list()) {
      ss << type_shorty(arg);
    }
  }
  auto type_string = ss.str();
  return DexString::make_string(type_string.c_str());
}

DexProto* DexProto::make_proto(DexType* rtype, DexTypeList* args) {
  auto shorty = make_shorty(rtype, args);
  return DexProto::make_proto(rtype, args, shorty);
}

void DexProto::gather_types(std::vector<DexType*>& ltype) {
  if (m_args) {
    m_args->gather_types(ltype);
  }
  if (m_rtype) {
    ltype.push_back(m_rtype);
  }
}

void DexProto::gather_strings(std::vector<DexString*>& lstring) {
  if (m_shorty) {
    lstring.push_back(m_shorty);
  }
}

void DexClass::gather_types(std::vector<DexType*>& ltype) {
  for (auto const& m : m_dmethods) {
    m->gather_types(ltype);
  }
  for (auto const& m : m_vmethods) {
    m->gather_types(ltype);
  }
  for (auto const& f : m_sfields) {
    f->gather_types(ltype);
  }
  for (auto const& f : m_ifields) {
    f->gather_types(ltype);
  }
  ltype.push_back(m_super_class);
  ltype.push_back(m_self);
  if (m_interfaces) m_interfaces->gather_types(ltype);
  if (m_anno) m_anno->gather_types(ltype);
}

void DexClass::gather_strings(std::vector<DexString*>& lstring) {
  for (auto const& m : m_dmethods) {
    m->gather_strings(lstring);
  }
  for (auto const& m : m_vmethods) {
    m->gather_strings(lstring);
  }
  for (auto const& f : m_sfields) {
    f->gather_strings(lstring);
  }
  for (auto const& f : m_ifields) {
    f->gather_strings(lstring);
  }
  if (m_source_file) lstring.push_back(m_source_file);
  if (m_anno) m_anno->gather_strings(lstring);
}

void DexClass::gather_fields(std::vector<DexField*>& lfield) {
  for (auto const& m : m_dmethods) {
    m->gather_fields(lfield);
  }
  for (auto const& m : m_vmethods) {
    m->gather_fields(lfield);
  }
  for (auto const& f : m_sfields) {
    lfield.push_back(f);
    f->gather_fields(lfield);
  }
  for (auto const& f : m_ifields) {
    lfield.push_back(f);
    f->gather_fields(lfield);
  }
  if (m_anno) m_anno->gather_fields(lfield);
}

void DexClass::gather_methods(std::vector<DexMethod*>& lmethod) {
  for (auto const& m : m_dmethods) {
    lmethod.push_back(m);
    m->gather_methods(lmethod);
  }
  for (auto const& m : m_vmethods) {
    lmethod.push_back(m);
    m->gather_methods(lmethod);
  }
  for (auto const& f : m_sfields) {
    f->gather_methods(lmethod);
  }
  for (auto const& f : m_ifields) {
    f->gather_methods(lmethod);
  }
  if (m_anno) m_anno->gather_methods(lmethod);
}

void DexField::gather_types(std::vector<DexType*>& ltype) {
  if (m_value) m_value->gather_types(ltype);
  if (m_anno) m_anno->gather_types(ltype);
}

void DexField::gather_strings(std::vector<DexString*>& lstring) {
  if (m_value) m_value->gather_strings(lstring);
  if (m_anno) m_anno->gather_strings(lstring);
}

void DexField::gather_fields(std::vector<DexField*>& lfield) {
  if (m_value) m_value->gather_fields(lfield);
  if (m_anno) m_anno->gather_fields(lfield);
}

void DexField::gather_methods(std::vector<DexMethod*>& lmethod) {
  if (m_value) m_value->gather_methods(lmethod);
  if (m_anno) m_anno->gather_methods(lmethod);
}

void DexField::gather_types_shallow(std::vector<DexType*>& ltype) {
  ltype.push_back(m_ref.cls);
  ltype.push_back(m_ref.type);
}

void DexField::gather_strings_shallow(std::vector<DexString*>& lstring) {
  lstring.push_back(m_ref.name);
}

void DexMethod::gather_types(std::vector<DexType*>& ltype) {
  // We handle m_ref.cls and proto in the first-layer gather.
  if (m_code) m_code->gather_types(ltype);
  if (m_anno) m_anno->gather_types(ltype);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto pair : *param_anno) {
      auto anno_set = pair.second;
      anno_set->gather_types(ltype);
    }
  }
}

void DexMethod::gather_strings(std::vector<DexString*>& lstring) {
  // We handle m_name and proto in the first-layer gather.
  if (m_code) m_code->gather_strings(lstring);
  if (m_anno) m_anno->gather_strings(lstring);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto pair : *param_anno) {
      auto anno_set = pair.second;
      anno_set->gather_strings(lstring);
    }
  }
}

void DexMethod::gather_fields(std::vector<DexField*>& lfield) {
  if (m_code) m_code->gather_fields(lfield);
  if (m_anno) m_anno->gather_fields(lfield);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto pair : *param_anno) {
      auto anno_set = pair.second;
      anno_set->gather_fields(lfield);
    }
  }
}

void DexMethod::gather_methods(std::vector<DexMethod*>& lmethod) {
  if (m_code) m_code->gather_methods(lmethod);
  if (m_anno) m_anno->gather_methods(lmethod);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto pair : *param_anno) {
      auto anno_set = pair.second;
      anno_set->gather_methods(lmethod);
    }
  }
}

void DexMethod::gather_types_shallow(std::vector<DexType*>& ltype) {
  ltype.push_back(m_ref.cls);
  m_ref.proto->gather_types(ltype);
}

void DexMethod::gather_strings_shallow(std::vector<DexString*>& lstring) {
  lstring.push_back(m_ref.name);
  m_ref.proto->gather_strings(lstring);
}

void DexCode::gather_catch_types(std::vector<DexType*>& ltype) {
  for (auto& tryit : m_tries) {
    for (auto const& it : tryit->m_catches) {
      if (it.first) {
        ltype.push_back(it.first);
      }
    }
  }
}

void DexCode::gather_types(std::vector<DexType*>& ltype) {
  for (auto const& opc : get_instructions()) {
    opc->gather_types(ltype);
  }
  gather_catch_types(ltype);
  if (m_dbg) m_dbg->gather_types(ltype);
}

void DexCode::gather_strings(std::vector<DexString*>& lstring) {
  for (auto const& opc : get_instructions()) {
    opc->gather_strings(lstring);
  }
  if (m_dbg) m_dbg->gather_strings(lstring);
}

void DexCode::gather_fields(std::vector<DexField*>& lfield) {
  for (auto const& opc : get_instructions()) {
    opc->gather_fields(lfield);
  }
}

void DexCode::gather_methods(std::vector<DexMethod*>& lmethod) {
  for (auto const& opc : get_instructions()) {
    opc->gather_methods(lmethod);
  }
}

std::string show_short(const DexMethod* p) {
  if (!p) return "";
  std::stringstream ss;
  ss << p->get_class()->get_name()->c_str() << "." << p->get_name()->c_str();
  return ss.str();
}
