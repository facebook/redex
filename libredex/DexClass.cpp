/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"

#include "Debug.h"
#include "DexAccess.h"
#include "DexDebugInstruction.h"
#include "DexDefs.h"
#include "DexMemberRefs.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "StringBuilder.h"
#include "Util.h"
#include "Walkers.h"
#include "Warning.h"

#include <algorithm>
#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>


uint32_t DexString::length() const {
  if (is_simple()) {
    return size();
  }
  return length_of_utf8_string(c_str());
}

int DexTypeList::encode(DexOutputIdx* dodx, uint32_t* output) {
  uint16_t* typep = (uint16_t*)(output + 1);
  *output = (uint32_t)m_list.size();
  for (auto const& type : m_list) {
    *typep++ = dodx->typeidx(type);
  }
  return (int)(((uint8_t*)typep) - (uint8_t*)output);
}

void DexField::make_concrete(DexAccessFlags access_flags, DexEncodedValue* v) {
  // FIXME assert if already concrete
  m_value = v;
  m_access = access_flags;
  m_concrete = true;
}

DexFieldRef* DexField::get_field(const std::string& full_descriptor) {
  auto fdt = dex_member_refs::parse_field(full_descriptor);
  auto cls = DexType::get_type(fdt.cls.c_str());
  auto name = DexString::get_string(fdt.name);
  auto type = DexType::get_type(fdt.type.c_str());
  return DexField::get_field(cls, name, type);
}

DexFieldRef* DexField::make_field(const std::string& full_descriptor) {
  auto fdt = dex_member_refs::parse_field(full_descriptor);
  auto cls = DexType::make_type(fdt.cls.c_str());
  auto name = DexString::make_string(fdt.name);
  auto type = DexType::make_type(fdt.type.c_str());
  return DexField::make_field(cls, name, type);
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
    uint32_t absolute_line) {
  std::vector<DexDebugEntry> entries;
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
      entries.emplace_back(pc, std::make_unique<DexPosition>(absolute_line));
      break;
    }
    }
  }
  return entries;
}

DexDebugItem::DexDebugItem(DexIdx* idx, uint32_t offset) {
  const uint8_t* encdata = idx->get_uleb_data(offset);
  uint32_t line_start = read_uleb128(&encdata);
  uint32_t paramcount = read_uleb128(&encdata);
  while (paramcount--) {
    DexString* str = decode_noindexable_string(idx, encdata);
    m_param_names.push_back(str);
  }
  std::vector<std::unique_ptr<DexDebugInstruction>> insns;
  DexDebugInstruction* dbgp;
  while ((dbgp = DexDebugInstruction::make_instruction(idx, &encdata)) !=
         nullptr) {
    insns.emplace_back(dbgp);
  }
  m_dbg_entries = eval_debug_instructions(this, insns, line_start);
}

uint32_t DexDebugItem::get_line_start() const {
  for (auto& entry : m_dbg_entries) {
    switch (entry.type) {
    case DexDebugEntryType::Position: {
      return entry.pos->line;
    default:
      break;
    }
    }
  }
  return 0;
}

DexDebugItem::DexDebugItem(const DexDebugItem& that)
    : m_param_names(that.m_param_names) {
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

std::unique_ptr<DexDebugItem> DexDebugItem::get_dex_debug(DexIdx* idx,
                                                          uint32_t offset) {
  if (offset == 0) return nullptr;
  return std::unique_ptr<DexDebugItem>(new DexDebugItem(idx, offset));
}

/*
 * Convert DexDebugEntries into debug opcodes.
 */
std::vector<std::unique_ptr<DexDebugInstruction>> generate_debug_instructions(
    DexDebugItem* debugitem,
    PositionMapper* pos_mapper,
    uint32_t* line_start,
    std::vector<DebugLineItem>* line_info) {
  std::vector<std::unique_ptr<DexDebugInstruction>> dbgops;
  uint32_t prev_addr = 0;
  boost::optional<uint32_t> prev_line;
  auto& entries = debugitem->get_entries();

  for (auto it = entries.begin(); it != entries.end(); ++it) {
    // find all entries that belong to the same address, and group them by type
    auto addr = it->addr;
    std::vector<DexPosition*> positions;
    std::vector<DexDebugInstruction*> insns;
    for (; it != entries.end() && it->addr == addr; ++it) {
      switch (it->type) {
      case DexDebugEntryType::Position:
        if (it->pos->file != nullptr) {
          positions.push_back(it->pos.get());
        }
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
      line_info->emplace_back(DebugLineItem(it->addr, line));
      int32_t line_delta;
      if (prev_line) {
        line_delta = line - *prev_line;
      } else {
        *line_start = line;
        line_delta = 0;
      }
      prev_line = line;
      if (line_delta < DBG_LINE_BASE ||
          line_delta >= (DBG_LINE_RANGE + DBG_LINE_BASE)) {
        dbgops.emplace_back(
            new DexDebugInstruction(DBG_ADVANCE_LINE, line_delta));
        line_delta = 0;
      }
      auto special = (line_delta - DBG_LINE_BASE) +
                     (addr_delta * DBG_LINE_RANGE) + DBG_FIRST_SPECIAL;
      if (special & ~0xff) {
        dbgops.emplace_back(
            new DexDebugInstruction(DBG_ADVANCE_PC, uint32_t(addr_delta)));
        special = line_delta - DBG_LINE_BASE + DBG_FIRST_SPECIAL;
      }
      dbgops.emplace_back(
          new DexDebugInstruction(static_cast<DexDebugItemOpcode>(special)));
      line_delta = 0;
      addr_delta = 0;
    }

    for (auto insn : insns) {
      if (addr_delta != 0) {
        dbgops.emplace_back(
            new DexDebugInstruction(DBG_ADVANCE_PC, addr_delta));
        addr_delta = 0;
      }
      dbgops.emplace_back(insn->clone());
    }
  }
  return dbgops;
}

int DexDebugItem::encode(
    DexOutputIdx* dodx,
    uint8_t* output,
    uint32_t line_start,
    const std::vector<DexString*>& parameters,
    const std::vector<std::unique_ptr<DexDebugInstruction>>& dbgops) {
  uint8_t* encdata = output;
  encdata = write_uleb128(encdata, line_start);
  encdata = write_uleb128(encdata, (uint32_t)parameters.size());
  for (auto s : parameters) {
    if (s == nullptr) {
      encdata = write_uleb128p1(encdata, DEX_NO_INDEX);
      continue;
    }
    uint32_t idx = dodx->stringidx(s);
    encdata = write_uleb128p1(encdata, idx);
  }
  for (auto& dbgop : dbgops) {
    dbgop->encode(dodx, encdata);
  }
  encdata = write_uleb128(encdata, DBG_END_SEQUENCE);
  return (int)(encdata - output);
}

void DexDebugItem::bind_positions(DexMethod* method, DexString* file) {
  auto* method_str = DexString::make_string(show(method));
  for (auto& entry : m_dbg_entries) {
    switch (entry.type) {
    case DexDebugEntryType::Position:
      entry.pos->bind(method_str, file);
      break;
    case DexDebugEntryType::Instruction:
      break;
    }
  }
}

void DexDebugItem::gather_types(std::vector<DexType*>& ltype) const {
  for (auto& entry : m_dbg_entries) {
    entry.gather_types(ltype);
  }
}

void DexDebugItem::gather_strings(std::vector<DexString*>& lstring) const {
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

std::unique_ptr<DexCode> DexCode::get_dex_code(DexIdx* idx, uint32_t offset) {
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
      DexInstruction* dop = DexInstruction::make_instruction(idx, &cdata);
      always_assert_log(dop != nullptr,
                        "Failed to parse method at offset 0x%08x", offset);
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
  dc->m_dbg = DexDebugItem::get_dex_debug(idx, code->debug_info_off);
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
  code->insns_size = (uint32_t)(insns - ((uint16_t*)(code + 1)));
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
    always_assert(dextry->m_start_addr < code->insns_size);
    dti[tryno].start_addr = dextry->m_start_addr;
    always_assert(dextry->m_start_addr + dextry->m_insn_count <= code->insns_size);
    dti[tryno].insn_count = dextry->m_insn_count;
    if (catches_map.find(dextry->m_catches) == catches_map.end()) {
      catches_map[dextry->m_catches] = hemit - handler_base;
      size_t catchcount = dextry->m_catches.size();
      bool has_catchall = dextry->m_catches.back().first == nullptr;
      if (has_catchall) {
        // -1 because the catch-all address is last (without an address)
        catchcount = -(catchcount - 1);
      }
      hemit = write_sleb128(hemit, (int32_t)catchcount);
      for (auto const& cit : dextry->m_catches) {
        auto type = cit.first;
        auto catch_addr = cit.second;
        if (type != nullptr) {
          // Assumption: The only catch-all is at the end of the list
          hemit = write_uleb128(hemit, dodx->typeidx(type));
        }
        always_assert(catch_addr < code->insns_size);
        hemit = write_uleb128(hemit, catch_addr);
      }
    }
    dti[tryno].handler_off = catches_map.at(dextry->m_catches);
  }
  return (int)(hemit - ((uint8_t*)output));
}

DexMethod::DexMethod(DexType* type, DexString* name, DexProto* proto)
    : DexMethodRef(type, name, proto) {
  m_virtual = false;
  m_anno = nullptr;
  m_dex_code = nullptr;
  m_code = nullptr;
  m_access = static_cast<DexAccessFlags>(0);
}

DexMethod::~DexMethod() = default;

std::string DexMethod::get_simple_deobfuscated_name() const {
  auto full_name = get_deobfuscated_name();
  if (full_name.empty()) {
    // This comes up for redex-created methods.
    return std::string(c_str());
  }
  auto dot_pos = full_name.find(".");
  auto colon_pos = full_name.find(":");
  if (dot_pos == std::string::npos || colon_pos == std::string::npos) {
    return full_name;
  }
  return full_name.substr(dot_pos + 1, colon_pos - dot_pos - 1);
}

// Why? get_deobfuscated_name and show_deobfuscated are not enough. deobfuscated
// names could be empty, e.g., when Redex-created methods. So we need a better
// job. And proto and type are still obfuscated in some cases. We also implement
// show_deobfuscated for DexProto.
namespace {
std::string build_fully_deobfuscated_name(const DexMethod* m) {
  string_builders::StaticStringBuilder<5> b;
  DexClass* cls = type_class(m->get_class());
  if (cls == nullptr) {
    // Well, just for safety.
    b << "<null>";
  } else {
    b << std::string(cls->get_deobfuscated_name().empty()
                         ? cls->get_name()->str()
                         : cls->get_deobfuscated_name());
  }

  b << "." << m->get_simple_deobfuscated_name() << ":"
    << show_deobfuscated(m->get_proto());
  return b.str();
}
} // namespace

std::string DexMethod::get_fully_deobfuscated_name() const {
  if (get_deobfuscated_name() == show(this)) {
    return get_deobfuscated_name();
  }
  return build_fully_deobfuscated_name(this);
}

void DexMethod::set_code(std::unique_ptr<IRCode> code) {
  m_code = std::move(code);
}

void DexMethod::balloon() {
  redex_assert(m_code == nullptr);
  m_code = std::make_unique<IRCode>(this);
  m_dex_code.reset();
}

void DexMethod::sync() {
  redex_assert(m_dex_code == nullptr);
  m_dex_code = m_code->sync(this);
  m_code.reset();
}

size_t hash_value(const DexMethodSpec& r) {
  size_t seed = boost::hash<DexType*>()(r.cls);
  boost::hash_combine(seed, r.name);
  boost::hash_combine(seed, r.proto);
  return seed;
}

DexMethod* DexMethod::make_method_from(DexMethod* that,
                                       DexType* target_cls,
                                       DexString* name) {
  auto m = static_cast<DexMethod*>(
      DexMethod::make_method(target_cls, name, that->get_proto()));
  redex_assert(m != that);
  if (that->m_anno) {
    m->m_anno = new DexAnnotationSet(*that->m_anno);
  }

  m->set_code(std::make_unique<IRCode>(*that->get_code()));

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

DexMethodRef* DexMethod::get_method(const std::string& full_descriptor) {
  auto mdt = dex_member_refs::parse_method(full_descriptor);
  auto cls = DexType::get_type(mdt.cls.c_str());
  auto name = DexString::get_string(mdt.name);
  std::deque<DexType*> args;
  for (auto& arg_str : mdt.args) {
    args.push_back(DexType::get_type(arg_str.c_str()));
  }
  auto dtl = DexTypeList::get_type_list(std::move(args));
  auto rtype = DexType::get_type(mdt.rtype.c_str());
  return DexMethod::get_method(cls, name, DexProto::get_proto(rtype, dtl));
}

DexMethodRef* DexMethod::make_method(const std::string& full_descriptor) {
  auto mdt = dex_member_refs::parse_method(full_descriptor);
  auto cls = DexType::make_type(mdt.cls.c_str());
  auto name = DexString::make_string(mdt.name);
  std::deque<DexType*> args;
  for (auto& arg_str : mdt.args) {
    args.push_back(DexType::make_type(arg_str.c_str()));
  }
  auto dtl = DexTypeList::make_type_list(std::move(args));
  auto rtype = DexType::make_type(mdt.rtype.c_str());
  return DexMethod::make_method(cls, name, DexProto::make_proto(rtype, dtl));
}

DexMethodRef* DexMethod::make_method(
    const std::string& class_type,
    const std::string& name,
    std::initializer_list<std::string> arg_types,
    const std::string& return_type) {
  std::deque<DexType*> dex_types;
  for (const std::string& type_str : arg_types) {
    dex_types.push_back(DexType::make_type(type_str.c_str()));
  }
  return DexMethod::make_method(
      DexType::make_type(class_type.c_str()),
      DexString::make_string(name),
      DexProto::make_proto(DexType::make_type(return_type.c_str()),
                           DexTypeList::make_type_list(std::move(dex_types))));
}

void DexClass::set_deobfuscated_name(const std::string& name) {
  // If the class has an old deobfuscated_name which is not equal to
  // `show(self)`, erase the name mapping from the global type map.
  if (!m_deobfuscated_name.empty()) {
    auto old_name = DexString::make_string(m_deobfuscated_name);
    if (old_name != m_self->get_name()) {
      g_redex->remove_type_name(old_name);
    }
  }
  m_deobfuscated_name = name;
  auto new_name = DexString::make_string(m_deobfuscated_name);
  if (new_name == m_self->get_name()) {
    return;
  }
  auto existing_type = g_redex->get_type(new_name);
  if (existing_type != nullptr) {
    fprintf(stderr,
            "Unable to alias type '%s' to deobfuscated name '%s' because type "
            "'%s' already exists.\n",
            m_self->c_str(),
            new_name->c_str(),
            existing_type->c_str());
    return;
  }
  g_redex->alias_type_name(m_self, new_name);
}

void DexClass::remove_method(const DexMethod* m) {
  auto& meths = m->is_virtual() ? m_vmethods : m_dmethods;
  auto it = std::find(meths.begin(), meths.end(), m);
  DEBUG_ONLY bool erased = false;
  if (it != meths.end()) {
    erased = true;
    meths.erase(it);
  }
  redex_assert(erased);
}

void DexMethod::become_virtual() {
  redex_assert(!m_virtual);
  auto cls = type_class(m_spec.cls);
  redex_assert(!cls->is_external());
  cls->remove_method(this);
  m_virtual = true;
  auto& vmethods = cls->get_vmethods();
  insert_sorted(vmethods, this, compare_dexmethods);
}

void DexMethod::make_concrete(DexAccessFlags access,
                              std::unique_ptr<DexCode> dc,
                              bool is_virtual) {
  m_access = access;
  m_dex_code = std::move(dc);
  m_concrete = true;
  m_virtual = is_virtual;
}

void DexMethod::make_concrete(DexAccessFlags access,
                              std::unique_ptr<IRCode> dc,
                              bool is_virtual) {
  m_access = access;
  m_code = std::move(dc);
  m_concrete = true;
  m_virtual = is_virtual;
}

void DexMethod::make_concrete(DexAccessFlags access, bool is_virtual) {
  make_concrete(access, std::unique_ptr<IRCode>(nullptr), is_virtual);
}

void DexMethod::make_non_concrete() {
  m_access = static_cast<DexAccessFlags>(0);
  m_concrete = false;
  m_code.reset();
  m_virtual = false;
  m_param_anno.clear();
}

/*
 * See class_data_item in Dex spec.
 */
void DexClass::load_class_data_item(DexIdx* idx,
                                    uint32_t cdi_off,
                                    DexEncodedValueArray* svalues) {
  if (cdi_off == 0) return;
  const uint8_t* encd = idx->get_uleb_data(cdi_off);
  uint32_t sfield_count = read_uleb128(&encd);
  uint32_t ifield_count = read_uleb128(&encd);
  uint32_t dmethod_count = read_uleb128(&encd);
  uint32_t vmethod_count = read_uleb128(&encd);
  uint32_t ndex = 0;
  for (uint32_t i = 0; i < sfield_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    DexField* df = static_cast<DexField*>(idx->get_fieldidx(ndex));
    DexEncodedValue* ev = nullptr;
    if (svalues != nullptr) {
      ev = svalues->pop_next();
    }
    // The last contiguous block of static fields with null values are not
    // represented in the encoded value array, so ev would be null for them.
    // OTOH null-initialized static fields that appear earlier in the static
    // field list have explicit values. Let's standardize things here.
    if (ev == nullptr) {
      ev = DexEncodedValue::zero_for_type(df->get_type());
    }
    df->make_concrete(access_flags, ev);
    m_sfields.push_back(df);
  }
  ndex = 0;
  for (uint32_t i = 0; i < ifield_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    DexField* df = static_cast<DexField*>(idx->get_fieldidx(ndex));
    df->make_concrete(access_flags);
    m_ifields.push_back(df);
  }

  std::unordered_set<DexMethod*> method_pointer_cache;

  ndex = 0;
  for (uint32_t i = 0; i < dmethod_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    uint32_t code_off = read_uleb128(&encd);
    // Find method in method index, returns same pointer for same method.
    DexMethod* dm = static_cast<DexMethod*>(idx->get_methodidx(ndex));
    std::unique_ptr<DexCode> dc = DexCode::get_dex_code(idx, code_off);
    if (dc && dc->get_debug_item()) {
      dc->get_debug_item()->bind_positions(dm, m_source_file);
    }
    dm->make_concrete(access_flags, std::move(dc), false);
    if (method_pointer_cache.count(dm)) {
      // found duplicate methods
      throw duplicate_method(SHOW(dm));
    } else {
      method_pointer_cache.insert(dm);
    }
    m_dmethods.push_back(dm);
  }
  ndex = 0;
  for (uint32_t i = 0; i < vmethod_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    uint32_t code_off = read_uleb128(&encd);
    // Find method in method index, returns same pointer for same method.
    DexMethod* dm = static_cast<DexMethod*>(idx->get_methodidx(ndex));
    auto dc = DexCode::get_dex_code(idx, code_off);
    if (dc && dc->get_debug_item()) {
      dc->get_debug_item()->bind_positions(dm, m_source_file);
    }
    dm->make_concrete(access_flags, std::move(dc), true);
    if (method_pointer_cache.count(dm)) {
      // found duplicate methods
      throw duplicate_method(SHOW(dm));
    } else {
      method_pointer_cache.insert(dm);
    }
    m_vmethods.push_back(dm);
  }
}

std::unique_ptr<IRCode> DexMethod::release_code() { return std::move(m_code); }

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

void DexClass::remove_field(const DexField* f) {
  bool is_static = f->get_access() & DexAccessFlags::ACC_STATIC;
  auto& fields = is_static ? m_sfields : m_ifields;
  DEBUG_ONLY bool erase = false;
  auto it = std::find(fields.begin(), fields.end(), f);
  if (it != fields.end()) {
    erase = true;
    fields.erase(it);
  }
  redex_assert(erase);
}

void DexClass::sort_fields() {
  auto& sfields = this->get_sfields();
  auto& ifields = this->get_ifields();
  std::sort(sfields.begin(), sfields.end(), compare_dexfields);
  std::sort(ifields.begin(), ifields.end(), compare_dexfields);
}

void DexClass::sort_methods() {
  auto& vmeths = this->get_vmethods();
  auto& dmeths = this->get_dmethods();
  std::sort(vmeths.begin(), vmeths.end(), compare_dexmethods);
  std::sort(dmeths.begin(), dmeths.end(), compare_dexmethods);
}

DexField* DexClass::find_field(const char* name,
                               const DexType* field_type) const {
  for (const auto f : m_ifields) {
    if (std::strcmp(f->c_str(), name) == 0 && f->get_type() == field_type) {
      return f;
    }
  }

  return nullptr;
}

bool DexClass::has_class_data() const {
  return !m_vmethods.empty() || !m_dmethods.empty() || !m_ifields.empty() ||
         !m_sfields.empty();
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

  sort_fields();
  sort_methods();

  uint8_t* encdata = output;
  encdata = write_uleb128(encdata, (uint32_t)m_sfields.size());
  encdata = write_uleb128(encdata, (uint32_t)m_ifields.size());
  encdata = write_uleb128(encdata, (uint32_t)m_dmethods.size());
  encdata = write_uleb128(encdata, (uint32_t)m_vmethods.size());
  uint32_t idxbase;
  idxbase = 0;
  for (auto const& f : m_sfields) {
    uint32_t idx = dodx->fieldidx(f);
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, f->get_access());
  }
  idxbase = 0;
  for (auto const& f : m_ifields) {
    uint32_t idx = dodx->fieldidx(f);
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
    redex_assert(!m->is_virtual());
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, m->get_access());
    uint32_t code_off = 0;
    if (m->get_dex_code() != nullptr && dco.count(m->get_dex_code())) {
      code_off = dco[m->get_dex_code()];
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
    redex_assert(m->is_virtual());
    encdata = write_uleb128(encdata, idx - idxbase);
    idxbase = idx;
    encdata = write_uleb128(encdata, m->get_access());
    uint32_t code_off = 0;
    if (m->get_dex_code() != nullptr && dco.count(m->get_dex_code())) {
      code_off = dco[m->get_dex_code()];
    }
    encdata = write_uleb128(encdata, code_off);
  }
  return (int)(encdata - output);
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
    DexField* field = static_cast<DexField*>(idx->get_fieldidx(fidx));
    DexAnnotationSet* aset = DexAnnotationSet::get_annotation_set(idx, off);
    field->attach_annotation_set(aset);
  }
  for (uint32_t i = 0; i < annodir->methods_size; i++) {
    uint32_t midx = *annodata++;
    uint32_t off = *annodata++;
    DexMethod* method = static_cast<DexMethod*>(idx->get_methodidx(midx));
    DexAnnotationSet* aset = DexAnnotationSet::get_annotation_set(idx, off);
    method->attach_annotation_set(aset);
  }
  for (uint32_t i = 0; i < annodir->parameters_size; i++) {
    uint32_t midx = *annodata++;
    uint32_t xrefoff = *annodata++;
    if (xrefoff != 0) {
      DexMethod* method = static_cast<DexMethod*>(idx->get_methodidx(midx));
      const uint32_t* annoxref = idx->get_uint_data(xrefoff);
      uint32_t count = *annoxref++;
      for (uint32_t j = 0; j < count; j++) {
        uint32_t off = annoxref[j];
        DexAnnotationSet* aset = DexAnnotationSet::get_annotation_set(idx, off);
        if (aset != nullptr) {
          method->attach_param_annotation_set(j, aset);
          redex_assert(method->get_param_anno());
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
  auto aev = std::make_unique<std::deque<DexEncodedValue*>>();
  for (auto it = m_sfields.rbegin(); it != m_sfields.rend(); ++it) {
    auto const& f = *it;
    DexEncodedValue* ev = f->get_static_value();
    if (!ev->is_zero() || has_static_values) {
      has_static_values = true;
      aev->push_front(ev);
    }
  }
  if (!has_static_values) return nullptr;
  return new DexEncodedValueArray(aev.release(), true);
}

DexAnnotationDirectory* DexClass::get_annotation_directory() {
  /* First scan to see what types of annotations to scan for if any.
   */
  std::unique_ptr<DexFieldAnnotations> fanno = nullptr;
  std::unique_ptr<DexMethodAnnotations> manno = nullptr;
  std::unique_ptr<DexMethodParamAnnotations> mpanno = nullptr;

  for (auto const& f : m_sfields) {
    if (f->get_anno_set()) {
      if (fanno == nullptr) {
        fanno = std::make_unique<DexFieldAnnotations>();
      }
      fanno->push_back(std::make_pair(f, f->get_anno_set()));
    }
  }
  for (auto const& f : m_ifields) {
    if (f->get_anno_set()) {
      if (fanno == nullptr) {
        fanno = std::make_unique<DexFieldAnnotations>();
      }
      fanno->push_back(std::make_pair(f, f->get_anno_set()));
    }
  }
  for (auto const& m : m_dmethods) {
    if (m->get_anno_set()) {
      if (manno == nullptr) {
        manno = std::make_unique<DexMethodAnnotations>();
      }
      manno->push_back(std::make_pair(m, m->get_anno_set()));
    }
    if (m->get_param_anno()) {
      if (mpanno == nullptr) {
        mpanno = std::make_unique<DexMethodParamAnnotations>();
      }
      mpanno->push_back(std::make_pair(m, m->get_param_anno()));
    }
  }
  for (auto const& m : m_vmethods) {
    if (m->get_anno_set()) {
      if (manno == nullptr) {
        manno = std::make_unique<DexMethodAnnotations>();
      }
      manno->push_back(std::make_pair(m, m->get_anno_set()));
    }
    if (m->get_param_anno()) {
      if (mpanno == nullptr) {
        mpanno = std::make_unique<DexMethodParamAnnotations>();
      }
      mpanno->push_back(std::make_pair(m, m->get_param_anno()));
    }
  }
  if (m_anno || fanno || manno || mpanno) {
    return new DexAnnotationDirectory(m_anno, std::move(fanno),
                                      std::move(manno), std::move(mpanno));
  }
  return nullptr;
}

DexClass::DexClass(DexIdx* idx,
                   const dex_class_def* cdef,
                   const std::string& location)
    : m_access_flags((DexAccessFlags)cdef->access_flags),
      m_super_class(idx->get_typeidx(cdef->super_idx)),
      m_self(idx->get_typeidx(cdef->typeidx)),
      m_interfaces(idx->get_type_list(cdef->interfaces_off)),
      m_source_file(idx->get_nullable_stringidx(cdef->source_file_idx)),
      m_anno(nullptr),
      m_external(false),
      m_perf_sensitive(false),
      m_location(location) {
  load_class_annotations(idx, cdef->annotations_off);
  auto deva = std::unique_ptr<DexEncodedValueArray>(
      load_static_values(idx, cdef->static_values_off));
  load_class_data_item(idx, cdef->class_data_offset, deva.get());
  g_redex->publish_class(this);
}

void DexTypeList::gather_types(std::vector<DexType*>& ltype) const {
  for (auto const& type : m_list) {
    ltype.push_back(type);
  }
}

static DexString* make_shorty(DexType* rtype, DexTypeList* args) {
  std::ostringstream ss;
  ss << type_shorty(rtype);
  if (args != nullptr) {
    for (auto arg : args->get_type_list()) {
      ss << type_shorty(arg);
    }
  }
  auto type_string = ss.str();
  return DexString::make_string(type_string);
}

DexProto* DexProto::make_proto(DexType* rtype, DexTypeList* args) {
  auto shorty = make_shorty(rtype, args);
  return DexProto::make_proto(rtype, args, shorty);
}

void DexProto::gather_types(std::vector<DexType*>& ltype) const {
  if (m_args) {
    m_args->gather_types(ltype);
  }
  if (m_rtype) {
    ltype.push_back(m_rtype);
  }
}

void DexProto::gather_strings(std::vector<DexString*>& lstring) const {
  if (m_shorty) {
    lstring.push_back(m_shorty);
  }
}

void DexClass::gather_types(std::vector<DexType*>& ltype) const {
  for (auto const& m : m_dmethods) {
    m->gather_types_shallow(ltype);
    m->gather_types(ltype);
  }
  for (auto const& m : m_vmethods) {
    m->gather_types_shallow(ltype);
    m->gather_types(ltype);
  }
  for (auto const& f : m_sfields) {
    f->gather_types_shallow(ltype);
    f->gather_types(ltype);
  }
  for (auto const& f : m_ifields) {
    f->gather_types_shallow(ltype);
    f->gather_types(ltype);
  }
  ltype.push_back(m_super_class);
  ltype.push_back(m_self);
  if (m_interfaces) m_interfaces->gather_types(ltype);
  if (m_anno) m_anno->gather_types(ltype);
}

void DexClass::gather_strings(std::vector<DexString*>& lstring,
                              bool exclude_loads) const {
  for (auto const& m : m_dmethods) {
    m->gather_strings(lstring, exclude_loads);
  }
  for (auto const& m : m_vmethods) {
    m->gather_strings(lstring, exclude_loads);
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

void DexClass::gather_fields(std::vector<DexFieldRef*>& lfield) const {
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

void DexClass::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
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

void DexFieldRef::gather_types_shallow(std::vector<DexType*>& ltype) const {
  ltype.push_back(m_spec.cls);
  ltype.push_back(m_spec.type);
}

void DexFieldRef::gather_strings_shallow(
    std::vector<DexString*>& lstring) const {
  lstring.push_back(m_spec.name);
}

void DexField::gather_types(std::vector<DexType*>& ltype) const {
  if (m_value) m_value->gather_types(ltype);
  if (m_anno) m_anno->gather_types(ltype);
}

void DexField::gather_strings(std::vector<DexString*>& lstring) const {
  if (m_value) m_value->gather_strings(lstring);
  if (m_anno) m_anno->gather_strings(lstring);
}

void DexField::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  if (m_value) m_value->gather_fields(lfield);
  if (m_anno) m_anno->gather_fields(lfield);
}

void DexField::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  if (m_value) m_value->gather_methods(lmethod);
  if (m_anno) m_anno->gather_methods(lmethod);
}

void DexMethod::gather_types(std::vector<DexType*>& ltype) const {
  // We handle m_spec.cls and proto in the first-layer gather.
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

void DexMethod::gather_strings(std::vector<DexString*>& lstring,
                               bool exclude_loads) const {
  // We handle m_name and proto in the first-layer gather.
  if (m_code && !exclude_loads) m_code->gather_strings(lstring);
  if (m_anno) m_anno->gather_strings(lstring);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto pair : *param_anno) {
      auto anno_set = pair.second;
      anno_set->gather_strings(lstring);
    }
  }
}

void DexMethod::gather_fields(std::vector<DexFieldRef*>& lfield) const {
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

void DexMethod::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
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

void DexMethodRef::gather_types_shallow(std::vector<DexType*>& ltype) const {
  ltype.push_back(m_spec.cls);
  m_spec.proto->gather_types(ltype);
}

void DexMethodRef::gather_strings_shallow(
    std::vector<DexString*>& lstring) const {
  lstring.push_back(m_spec.name);
  m_spec.proto->gather_strings(lstring);
}

uint32_t DexCode::size() const {
  uint32_t size = 0;
  for (auto const& opc : get_instructions()) {
    if (!dex_opcode::is_fopcode(opc->opcode())) {
      size += opc->size();
    }
  }
  return size;
}

void IRInstruction::gather_types(std::vector<DexType*>& ltype) const {
  switch (opcode::ref(opcode())) {
  case opcode::Ref::None:
  case opcode::Ref::String:
  case opcode::Ref::Literal:
  case opcode::Ref::Data:
    break;

  case opcode::Ref::Type:
    ltype.push_back(m_type);
    break;

  case opcode::Ref::Field:
    m_field->gather_types_shallow(ltype);
    break;

  case opcode::Ref::Method:
    m_method->gather_types_shallow(ltype);
    break;
  }
}

void gather_components(std::vector<DexString*>& lstring,
                       std::vector<DexType*>& ltype,
                       std::vector<DexFieldRef*>& lfield,
                       std::vector<DexMethodRef*>& lmethod,
                       const DexClasses& classes,
                       bool exclude_loads) {
  // Gather references reachable from each class.
  for (auto const& cls : classes) {
    cls->gather_strings(lstring, exclude_loads);
    cls->gather_types(ltype);
    cls->gather_fields(lfield);
    cls->gather_methods(lmethod);
  }

  // Remove duplicates to speed up the later loops.
  sort_unique(lstring);
  sort_unique(ltype);

  // Gather types and strings needed for field and method refs.
  sort_unique(lmethod);
  for (auto meth : lmethod) {
    meth->gather_types_shallow(ltype);
    meth->gather_strings_shallow(lstring);
  }

  sort_unique(lfield);
  for (auto field : lfield) {
    field->gather_types_shallow(ltype);
    field->gather_strings_shallow(lstring);
  }

  // Gather strings needed for each type.
  sort_unique(ltype);
  for (auto type : ltype) {
    if (type) lstring.push_back(type->get_name());
  }

  sort_unique(lstring);
}
