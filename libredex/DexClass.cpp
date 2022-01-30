/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"

#include "Debug.h"
#include "DexAccess.h"
#include "DexAnnotation.h"
#include "DexDebugInstruction.h"
#include "DexDefs.h"
#include "DexIdx.h"
#include "DexInstruction.h"
#include "DexMemberRefs.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "DuplicateClasses.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Show.h"
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
#include <unordered_set>

#define INSTANTIATE(METHOD, TYPE)                        \
  template void METHOD(std::vector<TYPE>&) const;        \
  template void METHOD(std::unordered_set<TYPE>&) const; \
  template void METHOD(std::vector<const TYPE>&) const;  \
  template void METHOD(std::unordered_set<const TYPE>&) const;

#define INSTANTIATE2(METHOD, TYPE, OTYPE)                       \
  template void METHOD(std::vector<TYPE>&, OTYPE) const;        \
  template void METHOD(std::unordered_set<TYPE>&, OTYPE) const; \
  template void METHOD(std::vector<const TYPE>&, OTYPE) const;  \
  template void METHOD(std::unordered_set<const TYPE>&, OTYPE) const;

namespace {

template <typename C, typename T>
struct InsertionHelper;

template <typename T>
struct InsertionHelper<std::vector<T>, T> {
  void append(std::vector<T>& c, T t) { c.emplace_back(std::move(t)); }
  template <typename It>
  void append_all(std::vector<T>& c, It first, It last) {
    c.insert(c.end(), first, last);
  }
};

template <typename T>
struct InsertionHelper<std::unordered_set<T>, T> {
  void append(std::unordered_set<T>& c, T t) { c.emplace(std::move(t)); }
  template <typename It>
  void append_all(std::unordered_set<T>& c, It first, It last) {
    c.insert(first, last);
  }
};

template <typename C>
void c_append(C& c, typename C::value_type t) {
  InsertionHelper<C, typename C::value_type>().append(c, t);
}
template <typename C, typename It>
void c_append_all(C& c, It first, It last) {
  InsertionHelper<C, typename C::value_type>().append_all(c, first, last);
}

} // namespace

namespace {
// Why? get_deobfuscated_name and show_deobfuscated are not enough. deobfuscated
// names could be empty, e.g., when Redex-created methods. So we need a better
// job. And proto and type are still obfuscated in some cases. We also implement
// show_deobfuscated for DexProto.
std::string build_fully_deobfuscated_name(const DexMethod* m) {
  string_builders::StaticStringBuilder<5> b;
  DexClass* cls = type_class(m->get_class());
  if (cls == nullptr) {
    // Well, just for safety.
    b << "<null>";
  } else {
    b << std::string(cls->get_deobfuscated_name_or_empty().empty()
                         ? cls->get_name()->str()
                         : cls->get_deobfuscated_name_or_empty());
  }

  b << "." << m->get_simple_deobfuscated_name() << ":"
    << show_deobfuscated(m->get_proto());
  return b.str();
}

// Return just the name of the method/field.
template <typename T>
std::string get_simple_deobf_name(const T* ref) {
  const auto& full_name = ref->get_deobfuscated_name_or_empty();
  if (full_name.empty()) {
    // This comes up for redex-created methods/fields.
    return std::string(ref->c_str());
  }
  auto dot_pos = full_name.find('.');
  auto colon_pos = full_name.find(':');
  if (dot_pos == std::string::npos || colon_pos == std::string::npos) {
    return full_name;
  }
  return full_name.substr(dot_pos + 1, colon_pos - dot_pos - 1);
}
} // namespace

const std::string DexString::EMPTY;

uint32_t DexString::length() const {
  if (is_simple()) {
    return size();
  }
  return length_of_utf8_string(c_str());
}

int32_t DexString::java_hashcode() const {
  return java_hashcode_of_utf8_string(c_str());
}

int DexTypeList::encode(DexOutputIdx* dodx, uint32_t* output) const {
  uint16_t* typep = (uint16_t*)(output + 1);
  *output = (uint32_t)m_list->size();
  for (auto const& type : *m_list) {
    *typep++ = dodx->typeidx(type);
  }
  return (int)(((uint8_t*)typep) - (uint8_t*)output);
}

DexTypeList* DexTypeList::push_front(DexType* t) const {
  ContainerType new_list;
  new_list.push_back(t);
  new_list.insert(new_list.end(), m_list->begin(), m_list->end());
  return make_type_list(std::move(new_list));
}

DexTypeList* DexTypeList::pop_front() const {
  redex_assert(!m_list->empty());
  ContainerType new_list{m_list->begin() + 1, m_list->end()};
  return make_type_list(std::move(new_list));
}
DexTypeList* DexTypeList::pop_front(size_t n) const {
  redex_assert(m_list->size() >= n);
  ContainerType new_list{m_list->begin() + n, m_list->end()};
  return make_type_list(std::move(new_list));
}

DexTypeList* DexTypeList::push_back(DexType* t) const {
  ContainerType new_list{*m_list};
  new_list.push_back(t);
  return make_type_list(std::move(new_list));
}
DexTypeList* DexTypeList::push_back(const std::vector<DexType*>& t) const {
  ContainerType new_list{*m_list};
  new_list.insert(new_list.end(), t.begin(), t.end());
  return make_type_list(std::move(new_list));
}

DexTypeList* DexTypeList::replace_head(DexType* new_head) const {
  redex_assert(!m_list->empty());
  ContainerType new_list{*m_list};
  new_list[0] = new_head;
  return make_type_list(std::move(new_list));
}

DexField::DexField(DexType* container, const DexString* name, DexType* type)
    : DexFieldRef(container, name, type),
      m_access(static_cast<DexAccessFlags>(0)),
      m_value(nullptr) {}
DexField::~DexField() = default; // For forwarding.

DexField* DexFieldRef::make_concrete(DexAccessFlags access_flags,
                                     DexEncodedValue* v) {
  // FIXME assert if already concrete
  auto that = static_cast<DexField*>(this);
  that->m_access = access_flags;
  that->m_concrete = true;
  if (is_static(access_flags)) {
    that->set_value(v);
  } else {
    always_assert(v == nullptr);
  }
  return that;
}

DexFieldRef* DexField::get_field(
    const dex_member_refs::FieldDescriptorTokens& fdt) {
  auto cls = DexType::get_type(fdt.cls);
  auto name = DexString::get_string(fdt.name);
  auto type = DexType::get_type(fdt.type);
  return DexField::get_field(cls, name, type);
}

DexFieldRef* DexField::get_field(std::string_view full_descriptor) {
  return get_field(dex_member_refs::parse_field(full_descriptor));
}

DexFieldRef* DexField::make_field(std::string_view full_descriptor) {
  auto fdt = dex_member_refs::parse_field(full_descriptor);
  auto cls = DexType::make_type(fdt.cls);
  auto name = DexString::make_string(fdt.name);
  auto type = DexType::make_type(fdt.type);
  return DexField::make_field(cls, name, type);
}

void DexField::set_external() {
  always_assert_log(!m_concrete, "Unexpected concrete field %s\n",
                    self_show().c_str());
  m_deobfuscated_name = DexString::make_string(self_show());
  m_external = true;
}

void DexField::set_value(DexEncodedValue* v) {
  always_assert_log(
      m_concrete,
      "Field needs to be concrete to be attached an encoded value.");
  always_assert(is_static(m_access));
  // The last contiguous block of static fields with null values are not
  // represented in the encoded value array. OTOH null-initialized static
  // fields that appear earlier in the static field list have explicit values.
  // Let's standardize things here.
  m_value = v != nullptr ? v : DexEncodedValue::zero_for_type(get_type());
}

void DexField::clear_annotations() { m_anno.reset(); }

void DexField::attach_annotation_set(std::unique_ptr<DexAnnotationSet> aset) {
  always_assert_type_log(!m_concrete, RedexError::BAD_ANNOTATION,
                         "field %s.%s is concrete\n",
                         m_spec.cls->get_name()->c_str(), m_spec.name->c_str());
  always_assert_type_log(!m_anno, RedexError::BAD_ANNOTATION,
                         "field %s.%s annotation exists\n",
                         m_spec.cls->get_name()->c_str(), m_spec.name->c_str());

  m_anno = std::move(aset);
}

std::unique_ptr<DexAnnotationSet> DexField::release_annotations() {
  return std::move(m_anno);
}

DexDebugEntry::DexDebugEntry(uint32_t addr,
                             std::unique_ptr<DexDebugInstruction> insn)
    : type(DexDebugEntryType::Instruction), addr(addr), insn(std::move(insn)) {}

DexDebugEntry::DexDebugEntry(uint32_t addr, std::unique_ptr<DexPosition> pos)
    : type(DexDebugEntryType::Position), addr(addr), pos(std::move(pos)) {}

DexDebugEntry::DexDebugEntry(DexDebugEntry&& other) noexcept
    : type(other.type), addr(other.addr) {
  switch (type) {
  case DexDebugEntryType::Position:
    new (&pos) std::unique_ptr<DexPosition>(std::move(other.pos));
    break;
  case DexDebugEntryType::Instruction:
    new (&insn) std::unique_ptr<DexDebugInstruction>(std::move(other.insn));
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

void DexDebugEntry::gather_strings(
    std::vector<const DexString*>& lstring) const {
  if (type == DexDebugEntryType::Instruction) {
    insn->gather_strings(lstring);
  }
}
void DexDebugEntry::gather_types(std::vector<DexType*>& ltype) const {
  if (type == DexDebugEntryType::Instruction) {
    insn->gather_types(ltype);
  }
}

/*
 * Evaluate the debug opcodes to figure out their absolute addresses and line
 * numbers.
 */
static std::vector<DexDebugEntry> eval_debug_instructions(
    DexDebugItem* dbg,
    DexIdx* idx,
    const uint8_t** encdata_ptr,
    uint32_t absolute_line) {
  std::vector<DexDebugEntry> entries;
  uint32_t pc = 0;
  while (true) {
    std::unique_ptr<DexDebugInstruction> opcode(
        DexDebugInstruction::make_instruction(idx, encdata_ptr));
    if (opcode == nullptr) {
      break;
    }
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

DexDebugItem::DexDebugItem(DexIdx* idx, uint32_t offset)
    : m_source_checksum(idx->get_checksum()), m_source_offset(offset) {
  const uint8_t* encdata = idx->get_uleb_data(offset);
  const uint8_t* base_encdata = encdata;
  uint32_t line_start = read_uleb128(&encdata);
  uint32_t paramcount = read_uleb128(&encdata);
  while (paramcount--) {
    // We intentionally drop the parameter string name here because we don't
    // have a convenient representation of it, and our internal tooling doesn't
    // use this info anyway.
    // We emit matching number of nulls as method arguments at the end.
    decode_noindexable_string(idx, encdata);
  }
  m_dbg_entries = eval_debug_instructions(this, idx, &encdata, line_start);
  m_on_disk_size = encdata - base_encdata;
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

DexDebugItem::DexDebugItem(const DexDebugItem& that) {
  std::unordered_map<DexPosition*, DexPosition*> pos_map;
  m_dbg_entries.reserve(that.m_dbg_entries.size());
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
    std::vector<DebugLineItem>* line_info,
    uint32_t line_addin) {
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
      auto line_base = pos_mapper->position_to_line(positions.back());
      auto line = line_base | line_addin;
      line_info->emplace_back(DebugLineItem(it->addr, line_base));
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
    uint32_t num_params,
    const std::vector<std::unique_ptr<DexDebugInstruction>>& dbgops) {
  uint8_t* encdata = output;
  encdata = write_uleb128(encdata, line_start);
  encdata = write_uleb128(encdata, num_params);
  for (uint32_t i = 0; i < num_params; ++i) {
    encdata = write_uleb128p1(encdata, DEX_NO_INDEX);
  }
  for (auto& dbgop : dbgops) {
    dbgop->encode(dodx, encdata);
  }
  encdata = write_uleb128(encdata, DBG_END_SEQUENCE);
  return (int)(encdata - output);
}

void DexDebugItem::bind_positions(DexMethod* method, const DexString* file) {
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

void DexDebugItem::gather_strings(
    std::vector<const DexString*>& lstring) const {
  for (auto& entry : m_dbg_entries) {
    entry.gather_strings(lstring);
  }
}

DexCode::DexCode(const DexCode& that)
    : m_registers_size(that.m_registers_size),
      m_ins_size(that.m_ins_size),
      m_outs_size(that.m_outs_size),
      m_insns(that.m_insns ? std::make_optional<std::vector<DexInstruction*>>()
                           : std::nullopt) {
  if (that.m_insns) {
    for (auto& insn : *that.m_insns) {
      m_insns->emplace_back(insn->clone());
    }
  }
  for (auto& try_ : that.m_tries) {
    m_tries.emplace_back(new DexTryItem(*try_));
  }
  if (that.m_dbg) {
    m_dbg.reset(new DexDebugItem(*that.m_dbg));
  }
}

DexCode::~DexCode() {
  if (m_insns) {
    for (auto const& op : *m_insns) {
      delete op;
    }
  }
}

std::unique_ptr<DexCode> DexCode::get_dex_code(DexIdx* idx, uint32_t offset) {
  if (offset == 0) return std::unique_ptr<DexCode>();
  const dex_code_item* code = (const dex_code_item*)idx->get_uint_data(offset);
  std::unique_ptr<DexCode> dc(new DexCode());
  dc->m_registers_size = code->registers_size;
  dc->m_ins_size = code->ins_size;
  dc->m_outs_size = code->outs_size;
  dc->m_insns = std::vector<DexInstruction*>();
  const uint16_t* cdata = (const uint16_t*)(code + 1);
  uint32_t tries = code->tries_size;
  if (code->insns_size) {
    // On average there seem to be about two code units per instruction
    dc->m_insns->reserve(code->insns_size / 2);
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
  if (m_tries.empty())
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
    always_assert(dextry->m_start_addr + dextry->m_insn_count <=
                  code->insns_size);
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

DexMethod::DexMethod(DexType* type, const DexString* name, DexProto* proto)
    : DexMethodRef(type, name, proto) {
  m_virtual = false;
  m_anno = nullptr;
  m_dex_code = nullptr;
  m_code = nullptr;
  m_access = static_cast<DexAccessFlags>(0);
}

DexMethod::~DexMethod() = default;

std::string DexMethod::get_fully_deobfuscated_name() const {
  if (m_deobfuscated_name != nullptr &&
      get_deobfuscated_name().str() == show(this)) {
    return get_deobfuscated_name().str();
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
                                       const DexString* name) {
  auto m = static_cast<DexMethod*>(
      DexMethod::make_method(target_cls, name, that->get_proto()));
  redex_assert(m != that);
  if (that->m_anno) {
    m->m_anno = std::make_unique<DexAnnotationSet>(*that->m_anno);
  }

  if (!is_abstract(that)) {
    always_assert_log(that->get_code() != nullptr, "%s", vshow(that).c_str());
    m->set_code(std::make_unique<IRCode>(*that->get_code()));
  } else {
    redex_assert(that->get_code() == nullptr);
  }

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

DexMethod* DexMethod::make_full_method_from(DexMethod* that,
                                            DexType* target_cls,
                                            const DexString* name) {
  auto m = make_method_from(that, target_cls, name);
  m->rstate = that->rstate;
  return m;
}

DexMethodRef* DexMethod::get_method(
    const dex_member_refs::MethodDescriptorTokens& mdt) {
  auto cls = DexType::get_type(mdt.cls);
  auto name = DexString::get_string(mdt.name);
  DexTypeList::ContainerType args;
  for (auto& arg_str : mdt.args) {
    args.push_back(DexType::get_type(arg_str));
  }
  auto dtl = DexTypeList::get_type_list(std::move(args));
  auto rtype = DexType::get_type(mdt.rtype);
  return DexMethod::get_method(cls, name, DexProto::get_proto(rtype, dtl));
}

template <bool kCheckFormat>
DexMethodRef* DexMethod::get_method(std::string_view full_descriptor) {
  return get_method(
      dex_member_refs::parse_method<kCheckFormat>(full_descriptor));
}
template DexMethodRef* DexMethod::get_method<false>(std::string_view);
template DexMethodRef* DexMethod::get_method<true>(std::string_view);

DexMethodRef* DexMethod::make_method(std::string_view full_descriptor) {
  auto mdt = dex_member_refs::parse_method(full_descriptor);
  auto cls = DexType::make_type(mdt.cls);
  auto name = DexString::make_string(mdt.name);
  DexTypeList::ContainerType args;
  for (auto& arg_str : mdt.args) {
    args.push_back(DexType::make_type(arg_str));
  }
  auto dtl = DexTypeList::make_type_list(std::move(args));
  auto rtype = DexType::make_type(mdt.rtype);
  return DexMethod::make_method(cls, name, DexProto::make_proto(rtype, dtl));
}

DexMethodRef* DexMethod::make_method(
    const std::string& class_type,
    const std::string& name,
    std::initializer_list<std::string> arg_types,
    const std::string& return_type) {
  DexTypeList::ContainerType dex_types;
  for (const std::string& type_str : arg_types) {
    dex_types.push_back(DexType::make_type(type_str.c_str()));
  }
  return DexMethod::make_method(
      DexType::make_type(class_type.c_str()),
      DexString::make_string(name),
      DexProto::make_proto(DexType::make_type(return_type.c_str()),
                           DexTypeList::make_type_list(std::move(dex_types))));
}

void DexMethod::combine_annotations_with(DexMethod* other) {
  auto other_anno_set = other->get_anno_set();
  if (other_anno_set != nullptr) {
    if (m_anno == nullptr) {
      m_anno = std::make_unique<DexAnnotationSet>(*other->m_anno);
    } else {
      m_anno->combine_with(*other->m_anno);
    }
  }
  for (auto& pair : other->m_param_anno) {
    if (m_param_anno.count(pair.first) == 0 ||
        m_param_anno[pair.first] == nullptr) {
      m_param_anno.emplace(pair.first, new DexAnnotationSet(*pair.second));
    } else {
      m_param_anno[pair.first]->combine_with(*pair.second);
    }
  }
}

void DexMethod::clear_annotations() { m_anno.reset(); }

void DexMethod::attach_annotation_set(std::unique_ptr<DexAnnotationSet> aset) {
  always_assert_type_log(!m_concrete, RedexError::BAD_ANNOTATION,
                         "method %s is concrete\n", self_show().c_str());
  always_assert_type_log(!m_anno, RedexError::BAD_ANNOTATION,
                         "method %s annotation exists\n", self_show().c_str());
  m_anno = std::move(aset);
}
void DexMethod::attach_param_annotation_set(
    int paramno, std::unique_ptr<DexAnnotationSet> aset) {
  always_assert_type_log(!m_concrete, RedexError::BAD_ANNOTATION,
                         "method %s is concrete\n", self_show().c_str());
  always_assert_type_log(m_param_anno.count(paramno) == 0,
                         RedexError::BAD_ANNOTATION,
                         "param %d annotation to method %s exists\n", paramno,
                         self_show().c_str());
  m_param_anno[paramno] = std::move(aset);
}

std::unique_ptr<DexAnnotationSet> DexMethod::release_annotations() {
  return std::move(m_anno);
}

void DexClass::set_deobfuscated_name(const std::string& name) {
  // If the class has an old deobfuscated_name which is not equal to
  // `show(self)`, erase the name mapping from the global type map.
  if (kInsertDeobfuscatedNameLinks && m_deobfuscated_name != nullptr) {
    if (m_deobfuscated_name != m_self->get_name()) {
      g_redex->remove_type_name(m_deobfuscated_name);
    }
  }
  m_deobfuscated_name = DexString::make_string(name);
  if (!kInsertDeobfuscatedNameLinks) {
    return;
  }
  if (m_deobfuscated_name == m_self->get_name()) {
    return;
  }
  auto existing_type = g_redex->get_type(m_deobfuscated_name);
  if (existing_type != nullptr) {
    TRACE(DC, 5,
          "Unable to alias type '%s' to deobfuscated name '%s' because type "
          "'%s' already exists.\n",
          m_self->c_str(), m_deobfuscated_name->c_str(),
          existing_type->c_str());
    return;
  }
  g_redex->alias_type_name(m_self, m_deobfuscated_name);
}

void DexClass::set_deobfuscated_name(const DexString* name) {
  // If the class has an old deobfuscated_name which is not equal to
  // `show(self)`, erase the name mapping from the global type map.
  if (kInsertDeobfuscatedNameLinks && m_deobfuscated_name != nullptr) {
    if (m_deobfuscated_name != m_self->get_name()) {
      g_redex->remove_type_name(m_deobfuscated_name);
    }
  }
  m_deobfuscated_name = name;
  if (!kInsertDeobfuscatedNameLinks) {
    return;
  }
  if (m_deobfuscated_name == m_self->get_name()) {
    return;
  }
  auto existing_type = g_redex->get_type(m_deobfuscated_name);
  if (existing_type != nullptr) {
    TRACE(DC, 5,
          "Unable to alias type '%s' to deobfuscated name '%s' because type "
          "'%s' already exists.\n",
          m_self->c_str(), m_deobfuscated_name->c_str(),
          existing_type->c_str());
    return;
  }
  g_redex->alias_type_name(m_self, m_deobfuscated_name);
}
void DexClass::set_deobfuscated_name(const DexString& name) {
  set_deobfuscated_name(&name);
}

void DexClass::set_external() {
  m_deobfuscated_name = DexString::make_string(self_show());
  m_external = true;
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

void DexClass::remove_method_definition(DexMethod* m) {
  remove_method(m);
  // Virtually delete the definition of the method.
  m->m_concrete = false;
  m->release_code();
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

DexMethod* DexMethodRef::make_concrete(DexAccessFlags access,
                                       std::unique_ptr<DexCode> dc,
                                       bool is_virtual) {
  auto that = static_cast<DexMethod*>(this);
  that->m_access = access;
  that->m_dex_code = std::move(dc);
  that->m_concrete = true;
  that->m_virtual = is_virtual;
  return that;
}

DexMethod* DexMethodRef::make_concrete(DexAccessFlags access,
                                       std::unique_ptr<IRCode> dc,
                                       bool is_virtual) {
  auto that = static_cast<DexMethod*>(this);
  that->m_access = access;
  that->m_code = std::move(dc);
  that->m_concrete = true;
  that->m_virtual = is_virtual;
  return that;
}

DexMethod* DexMethodRef::make_concrete(DexAccessFlags access, bool is_virtual) {
  return make_concrete(access, std::unique_ptr<IRCode>(nullptr), is_virtual);
}

void DexMethod::make_non_concrete() {
  m_access = static_cast<DexAccessFlags>(0);
  m_concrete = false;
  m_code.reset();
  m_virtual = false;
  m_param_anno.clear();
}

void DexMethod::set_deobfuscated_name(const std::string& name) {
  // If the method has an old deobfuscated_name which is not equal to the name,
  // erase the mapping using the old (and now invalid) deobfuscated_name from
  // the global type map.
  if (kInsertDeobfuscatedNameLinks && m_deobfuscated_name != nullptr &&
      !m_deobfuscated_name->str().empty()) {
    if (m_deobfuscated_name != this->get_name()) {
      g_redex->erase_method(this->get_class(), m_deobfuscated_name,
                            this->get_proto());
    }
  }
  m_deobfuscated_name = DexString::make_string(name);
  if (!kInsertDeobfuscatedNameLinks) {
    return;
  }
  if (m_deobfuscated_name == this->get_name()) {
    return;
  }
  auto existing_method = g_redex->get_method(
      this->get_class(), m_deobfuscated_name, this->get_proto());
  if (existing_method != nullptr) {
    TRACE(DC, 5,
          "Unable to alias method '%s' to deobfuscated name '%s' because "
          "field '%s' already exists.\n ",
          this->c_str(), m_deobfuscated_name->c_str(),
          existing_method->c_str());
    return;
  }
  g_redex->alias_method_name(this, m_deobfuscated_name);
}

std::string DexMethod::get_simple_deobfuscated_name() const {
  return get_simple_deobf_name(this);
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

  std::vector<DexEncodedValue*> empty{};
  std::vector<DexEncodedValue*>& used =
      (svalues == nullptr || svalues->evalues() == nullptr)
          ? empty
          : *svalues->evalues();
  auto it = used.begin();

  m_sfields.reserve(sfield_count);
  for (uint32_t i = 0; i < sfield_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    DexField* df = static_cast<DexField*>(idx->get_fieldidx(ndex));
    DexEncodedValue* ev = nullptr;
    if (it != used.end()) {
      ev = *it;
      ++it;
    }
    df->make_concrete(access_flags, ev);
    m_sfields.push_back(df);
  }
  ndex = 0;
  m_ifields.reserve(ifield_count);
  for (uint32_t i = 0; i < ifield_count; i++) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    DexField* df = static_cast<DexField*>(idx->get_fieldidx(ndex));
    df->make_concrete(access_flags);
    m_ifields.push_back(df);
  }

  std::unordered_set<DexMethod*> method_pointer_cache;
  method_pointer_cache.reserve(dmethod_count + vmethod_count);

  auto process_method = [this, &encd, &idx, &method_pointer_cache](
                            uint32_t& ndex, bool is_virtual) {
    ndex += read_uleb128(&encd);
    auto access_flags = (DexAccessFlags)read_uleb128(&encd);
    uint32_t code_off = read_uleb128(&encd);
    // Find method in method index, returns same pointer for same method.
    DexMethod* dm = static_cast<DexMethod*>(idx->get_methodidx(ndex));
    std::unique_ptr<DexCode> dc = DexCode::get_dex_code(idx, code_off);
    if (dc && dc->get_debug_item()) {
      dc->get_debug_item()->bind_positions(dm, m_source_file);
    }
    dm->make_concrete(access_flags, std::move(dc), is_virtual);

    const auto& pair = method_pointer_cache.insert(dm);
    bool insertion_happened = pair.second;
    always_assert_type_log(insertion_happened, RedexError::DUPLICATE_METHODS,
                           "Found duplicate methods in the same class. %s",
                           SHOW(dm));

    return dm;
  };

  m_dmethods.reserve(dmethod_count);
  ndex = 0;
  for (uint32_t i = 0; i < dmethod_count; i++) {
    DexMethod* dm = process_method(ndex, false);
    m_dmethods.push_back(dm);
  }
  m_vmethods.reserve(vmethod_count);
  ndex = 0;
  for (uint32_t i = 0; i < vmethod_count; i++) {
    DexMethod* dm = process_method(ndex, true);
    m_vmethods.push_back(dm);
  }
}

std::unique_ptr<IRCode> DexMethod::release_code() { return std::move(m_code); }

std::vector<DexMethod*> DexClass::get_all_methods() const {
  std::vector<DexMethod*> all_methods(m_vmethods.begin(), m_vmethods.end());
  all_methods.insert(all_methods.end(), m_dmethods.begin(), m_dmethods.end());
  return all_methods;
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

std::vector<DexField*> DexClass::get_all_fields() const {
  std::vector<DexField*> all_fields(m_ifields.begin(), m_ifields.end());
  all_fields.insert(all_fields.end(), m_sfields.begin(), m_sfields.end());
  return all_fields;
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

void DexClass::remove_field_definition(DexField* f) {
  remove_field(f);
  f->m_concrete = false;
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

DexField* DexClass::find_ifield(const char* name,
                                const DexType* field_type) const {
  for (const auto f : m_ifields) {
    if (std::strcmp(f->c_str(), name) == 0 && f->get_type() == field_type) {
      return f;
    }
  }

  return nullptr;
}

DexField* DexClass::find_sfield(const char* name,
                                const DexType* field_type) const {
  for (const auto f : m_sfields) {
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
  if (m_sfields.empty() && m_ifields.empty() && m_dmethods.empty() &&
      m_vmethods.empty()) {
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
    auto aset = DexAnnotationSet::get_annotation_set(idx, off);
    field->attach_annotation_set(std::move(aset));
  }
  for (uint32_t i = 0; i < annodir->methods_size; i++) {
    uint32_t midx = *annodata++;
    uint32_t off = *annodata++;
    DexMethod* method = static_cast<DexMethod*>(idx->get_methodidx(midx));
    auto aset = DexAnnotationSet::get_annotation_set(idx, off);
    method->attach_annotation_set(std::move(aset));
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
        auto aset = DexAnnotationSet::get_annotation_set(idx, off);
        if (aset != nullptr) {
          method->attach_param_annotation_set(j, std::move(aset));
          redex_assert(method->get_param_anno());
        }
      }
    }
  }
}

void DexClass::combine_annotations_with(DexClass* other) {
  auto other_anno_set = other->get_anno_set();
  if (other_anno_set != nullptr) {
    if (m_anno == nullptr) {
      m_anno = std::make_unique<DexAnnotationSet>(*other->m_anno);
    } else {
      m_anno->combine_with(*other->m_anno);
    }
  }
}

void DexClass::attach_annotation_set(std::unique_ptr<DexAnnotationSet> anno) {
  m_anno = std::move(anno);
}

void DexClass::clear_annotations() { m_anno.reset(); }

static DexEncodedValueArray* load_static_values(DexIdx* idx, uint32_t sv_off) {
  if (sv_off == 0) return nullptr;
  const uint8_t* encd = idx->get_uleb_data(sv_off);
  return get_encoded_value_array(idx, encd);
}

DexEncodedValueArray* DexClass::get_static_values() {
  std::deque<DexEncodedValue*> deque;
  for (auto it = m_sfields.rbegin(); it != m_sfields.rend(); ++it) {
    auto const& f = *it;
    DexEncodedValue* ev = f->get_static_value();
    if (!ev->is_zero() || !deque.empty()) {
      deque.push_front(ev);
    }
  }
  if (deque.empty()) {
    return nullptr;
  }

  auto aev = std::make_unique<std::vector<DexEncodedValue*>>(deque.begin(),
                                                             deque.end());
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
    return new DexAnnotationDirectory(m_anno.get(), std::move(fanno),
                                      std::move(manno), std::move(mpanno));
  }
  return nullptr;
}

DexClass* DexClass::create(DexIdx* idx,
                           const dex_class_def* cdef,
                           const std::string& location) {
  DexClass* cls = new DexClass(idx, cdef, location);
  if (g_redex->class_already_loaded(cls)) {
    // FIXME: This isn't deterministic. We're keeping whichever class we loaded
    // first, which may not always be from the same dex (if we load them in
    // parallel, for example).
    delete cls;
    return nullptr;
  }
  cls->load_class_annotations(idx, cdef->annotations_off);
  auto deva = std::unique_ptr<DexEncodedValueArray>(
      load_static_values(idx, cdef->static_values_off));
  cls->load_class_data_item(idx, cdef->class_data_offset, deva.get());
  g_redex->publish_class(cls);
  return cls;
}

DexClass::DexClass(const std::string& location) : m_location(location) {}

DexClass::DexClass(DexIdx* idx,
                   const dex_class_def* cdef,
                   const std::string& location)
    : m_super_class(idx->get_typeidx(cdef->super_idx)),
      m_self(idx->get_typeidx(cdef->typeidx)),
      m_interfaces(idx->get_type_list(cdef->interfaces_off)),
      m_source_file(idx->get_nullable_stringidx(cdef->source_file_idx)),
      m_anno(nullptr),
      m_location(location),
      m_access_flags((DexAccessFlags)cdef->access_flags),
      m_external(false),
      m_perf_sensitive(false) {}

DexClass::~DexClass() = default; // For forwarding.

template <typename C>
void DexTypeList::gather_types(C& ltype) const {
  c_append_all(ltype, m_list->begin(), m_list->end());
}
INSTANTIATE(DexTypeList::gather_types, DexType*)

static const DexString* make_shorty(const DexType* rtype,
                                    const DexTypeList* args) {
  std::string s;
  s.push_back(type::type_shorty(rtype));
  if (args != nullptr) {
    for (auto arg : *args) {
      s.push_back(type::type_shorty(arg));
    }
  }
  return DexString::make_string(s);
}

DexProto* DexProto::make_proto(const DexType* rtype, const DexTypeList* args) {
  auto shorty = make_shorty(rtype, args);
  return DexProto::make_proto(rtype, args, shorty);
}

template <typename C>
void DexProto::gather_types(C& ltype) const {
  if (m_args) {
    m_args->gather_types(ltype);
  }
  if (m_rtype) {
    c_append(ltype, m_rtype);
  }
}
INSTANTIATE(DexProto::gather_types, DexType*)

void DexProto::gather_strings(std::vector<const DexString*>& lstring) const {
  if (m_shorty) {
    c_append(lstring, m_shorty);
  }
}
void DexProto::gather_strings(
    std::unordered_set<const DexString*>& lstring) const {
  if (m_shorty) {
    c_append(lstring, m_shorty);
  }
}

namespace {

template <typename C>
void maybe_sort_unique(C&) {}
template <>
void maybe_sort_unique(std::vector<DexType*>& vec) {
  sort_unique(vec);
}

} // namespace

template <typename C>
void DexClass::gather_types(C& ltype) const {
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

  ltype.insert(ltype.end(), m_super_class);
  ltype.insert(ltype.end(), m_self);
  if (m_interfaces) m_interfaces->gather_types(ltype);
  if (m_anno) {
    std::vector<DexType*> type_vec;
    m_anno->gather_types(type_vec);
    c_append_all(ltype, type_vec.begin(), type_vec.end());
  }

  // We also need to gather types needed for field and method refs.
  std::vector<DexFieldRef*> lfield;
  gather_fields(lfield);
  for (auto const& f : lfield) {
    f->gather_types_shallow(ltype);
  }

  std::vector<DexMethodRef*> lmethod;
  gather_methods(lmethod);
  for (auto const& m : lmethod) {
    m->gather_types_shallow(ltype);
  }

  // Remove duplicates.
  maybe_sort_unique(ltype);
}
INSTANTIATE(DexClass::gather_types, DexType*)

void DexClass::gather_load_types(std::unordered_set<DexType*>& ltype) const {
  if (is_external()) {
    return;
  }
  if (ltype.count(m_self) != 0) {
    return;
  }
  ltype.emplace(m_self);
  {
    auto superclass = type_class_internal(m_super_class);
    if (superclass != nullptr) {
      superclass->gather_load_types(ltype);
    }
  }
  if (m_interfaces) {
    for (auto* itype : *m_interfaces) {
      auto iclass = type_class_internal(itype);
      if (iclass != nullptr) {
        iclass->gather_load_types(ltype);
      }
    }
  }
}

void DexClass::gather_init_classes(std::vector<DexType*>& ltype) const {
  for (auto const& m : m_dmethods) {
    m->gather_init_classes(ltype);
  }
  for (auto const& m : m_vmethods) {
    m->gather_init_classes(ltype);
  }
}

template <typename C>
void DexClass::gather_strings_internal(C& lstring, bool exclude_loads) const {
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
  if (m_source_file) c_append(lstring, m_source_file);
  if (m_anno) {
    std::vector<const DexString*> strings;
    m_anno->gather_strings(strings);
    c_append_all(lstring, strings.begin(), strings.end());
  }
}
void DexClass::gather_strings(std::vector<const DexString*>& lstring,
                              bool exclude_loads) const {
  gather_strings_internal(lstring, exclude_loads);
}
void DexClass::gather_strings(std::unordered_set<const DexString*>& lstring,
                              bool exclude_loads) const {
  gather_strings_internal(lstring, exclude_loads);
}

template <typename C>
void DexClass::gather_fields(C& lfield) const {
  for (auto const& m : m_dmethods) {
    m->gather_fields(lfield);
  }
  for (auto const& m : m_vmethods) {
    m->gather_fields(lfield);
  }
  for (auto const& f : m_sfields) {
    lfield.insert(lfield.end(), f);
    f->gather_fields(lfield);
  }
  for (auto const& f : m_ifields) {
    lfield.insert(lfield.end(), f);
    f->gather_fields(lfield);
  }
  if (m_anno) {
    std::vector<DexFieldRef*> fields_vec; // Simplify refactor.
    m_anno->gather_fields(fields_vec);
    c_append_all(lfield, fields_vec.begin(), fields_vec.end());
  }
}
INSTANTIATE(DexClass::gather_fields, DexFieldRef*)

template <typename C>
void DexClass::gather_methods(C& lmethod) const {
  for (auto const& m : m_dmethods) {
    lmethod.insert(lmethod.end(), m);
    m->gather_methods(lmethod);
  }
  for (auto const& m : m_vmethods) {
    lmethod.insert(lmethod.end(), m);
    m->gather_methods(lmethod);
  }
  for (auto const& f : m_sfields) {
    f->gather_methods(lmethod);
  }
  for (auto const& f : m_ifields) {
    f->gather_methods(lmethod);
  }
  if (m_anno) {
    std::vector<DexMethodRef*> method_vec; // Simplify refactor.
    m_anno->gather_methods(method_vec);
    c_append_all(lmethod, method_vec.begin(), method_vec.end());
  }
}
INSTANTIATE(DexClass::gather_methods, DexMethodRef*)

const DexField* DexFieldRef::as_def() const {
  if (is_def()) {
    return static_cast<const DexField*>(this);
  } else {
    return nullptr;
  }
}

DexField* DexFieldRef::as_def() {
  if (is_def()) {
    return static_cast<DexField*>(this);
  } else {
    return nullptr;
  }
}

// Find methods and fields from a class using its obfuscated name.
DexField* DexClass::find_field_from_simple_deobfuscated_name(
    const std::string& field_name) {
  for (DexField* f : get_sfields()) {
    if (f->get_simple_deobfuscated_name() == field_name) {
      return f;
    }
  }
  for (DexField* f : get_ifields()) {
    if (f->get_simple_deobfuscated_name() == field_name) {
      return f;
    }
  }
  return nullptr;
}

DexMethod* DexClass::find_method_from_simple_deobfuscated_name(
    const std::string& method_name) {
  for (DexMethod* m : get_dmethods()) {
    if (m->get_simple_deobfuscated_name() == method_name) {
      return m;
    }
  }
  for (DexMethod* m : get_vmethods()) {
    if (m->get_simple_deobfuscated_name() == method_name) {
      return m;
    }
  }
  return nullptr;
}

template <typename C>
void DexClass::gather_callsites(C& lcallsite) const {
  for (auto const& m : m_dmethods) {
    m->gather_callsites(lcallsite);
  }
  for (auto const& m : m_vmethods) {
    m->gather_callsites(lcallsite);
  }
}
INSTANTIATE(DexClass::gather_callsites, DexCallSite*)

template <typename C>
void DexClass::gather_methodhandles(C& lmethodhandle) const {
  for (auto const& m : m_dmethods) {
    m->gather_methodhandles(lmethodhandle);
  }
  for (auto const& m : m_vmethods) {
    m->gather_methodhandles(lmethodhandle);
  }
}
INSTANTIATE(DexClass::gather_methodhandles, DexMethodHandle*)

template <typename C>
void DexFieldRef::gather_types_shallow(C& ltype) const {
  ltype.insert(ltype.end(), m_spec.cls);
  ltype.insert(ltype.end(), m_spec.type);
}
INSTANTIATE(DexFieldRef::gather_types_shallow, DexType*)

void DexFieldRef::gather_strings_shallow(
    std::vector<const DexString*>& lstring) const {
  c_append(lstring, m_spec.name);
}
void DexFieldRef::gather_strings_shallow(
    std::unordered_set<const DexString*>& lstring) const {
  c_append(lstring, m_spec.name);
}

template <typename C>
void DexField::gather_types(C& ltype) const {
  std::vector<DexType*> type_vec;
  if (m_value) m_value->gather_types(type_vec);
  if (m_anno) m_anno->gather_types(type_vec);
  c_append_all(ltype, type_vec.begin(), type_vec.end());
}
INSTANTIATE(DexField::gather_types, DexType*)

template <typename C>
void DexField::gather_strings_internal(C& lstring) const {
  std::vector<const DexString*> string_vec;
  if (m_value) m_value->gather_strings(string_vec);
  if (m_anno) m_anno->gather_strings(string_vec);
  c_append_all(lstring, string_vec.begin(), string_vec.end());
}
void DexField::gather_strings(std::vector<const DexString*>& lstring) const {
  gather_strings_internal(lstring);
}
void DexField::gather_strings(
    std::unordered_set<const DexString*>& lstring) const {
  gather_strings_internal(lstring);
}

template <typename C>
void DexField::gather_fields(C& lfield) const {
  std::vector<DexFieldRef*> field_vec;
  if (m_value) m_value->gather_fields(field_vec);
  if (m_anno) m_anno->gather_fields(field_vec);
  c_append_all(lfield, field_vec.begin(), field_vec.end());
}
INSTANTIATE(DexField::gather_fields, DexFieldRef*)

template <typename C>
void DexField::gather_methods(C& lmethod) const {
  std::vector<DexMethodRef*> method_vec;
  if (m_value) m_value->gather_methods(method_vec);
  if (m_anno) m_anno->gather_methods(method_vec);
  c_append_all(lmethod, method_vec.begin(), method_vec.end());
}
INSTANTIATE(DexField::gather_methods, DexMethodRef*)

void DexField::set_deobfuscated_name(const std::string& name) {
  // If the field has an old deobfuscated_name which is not equal to the name,
  // erase the mapping using the old (and now invalid) deobfuscated_name from
  // the global type map.
  if (kInsertDeobfuscatedNameLinks && m_deobfuscated_name != nullptr) {
    if (m_deobfuscated_name != this->get_name()) {
      g_redex->erase_field(this->get_class(), m_deobfuscated_name,
                           this->get_type());
    }
  }
  m_deobfuscated_name = DexString::make_string(name);
  if (!kInsertDeobfuscatedNameLinks) {
    return;
  }
  if (m_deobfuscated_name == this->get_name()) {
    return;
  }
  auto existing_field = g_redex->get_field(
      this->get_class(), m_deobfuscated_name, this->get_type());
  if (existing_field != nullptr) {
    TRACE(DC, 5,
          "Unable to alias field '%s' to deobfuscated name '%s' because "
          "field '%s' already exists.\n ",
          this->c_str(), m_deobfuscated_name->c_str(), existing_field->c_str());
    return;
  }
  g_redex->alias_field_name(this, m_deobfuscated_name);
}

void DexField::set_deobfuscated_name(const DexString* name) {
  // If the field has an old deobfuscated_name which is not equal to the name,
  // erase the mapping using the old (and now invalid) deobfuscated_name from
  // the global type map.
  if (kInsertDeobfuscatedNameLinks && m_deobfuscated_name != nullptr) {
    if (m_deobfuscated_name != this->get_name()) {
      g_redex->erase_field(this->get_class(), m_deobfuscated_name,
                           this->get_type());
    }
  }
  m_deobfuscated_name = name;
  if (!kInsertDeobfuscatedNameLinks) {
    return;
  }
  if (m_deobfuscated_name == this->get_name()) {
    return;
  }
  auto existing_field = g_redex->get_field(
      this->get_class(), m_deobfuscated_name, this->get_type());
  if (existing_field != nullptr) {
    TRACE(DC, 5,
          "Unable to alias field '%s' to deobfuscated name '%s' because "
          "field '%s' already exists.\n ",
          this->c_str(), m_deobfuscated_name->c_str(), existing_field->c_str());
    return;
  }
  g_redex->alias_field_name(this, m_deobfuscated_name);
}

void DexField::set_deobfuscated_name(const DexString& name) {
  set_deobfuscated_name(&name);
}

std::string DexField::get_simple_deobfuscated_name() const {
  return get_simple_deobf_name(this);
}

void DexMethod::set_external() {
  always_assert_log(!m_concrete, "Unexpected concrete method %s\n",
                    self_show().c_str());
  m_deobfuscated_name = DexString::make_string(self_show());
  m_external = true;
}

template <typename C>
void DexMethod::gather_types(C& ltype) const {
  gather_types_shallow(ltype); // Handle DexMethodRef parts.
  std::vector<DexType*> type_vec; // Simplify refactor.
  if (m_code) m_code->gather_types(type_vec);
  if (m_anno) m_anno->gather_types(type_vec);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto& pair : *param_anno) {
      auto& anno_set = pair.second;
      anno_set->gather_types(type_vec);
    }
  }
  c_append_all(ltype, type_vec.begin(), type_vec.end());
}
INSTANTIATE(DexMethod::gather_types, DexType*)

void DexMethod::gather_init_classes(std::vector<DexType*>& ltype) const {
  if (m_code) m_code->gather_init_classes(ltype);
}

template <typename C>
void DexMethod::gather_callsites(C& lcallsite) const {
  // We handle m_spec.cls and proto in the first-layer gather.
  if (m_code) {
    std::vector<DexCallSite*> callsite_vec; // Simplify refactor.
    m_code->gather_callsites(callsite_vec);
    c_append_all(lcallsite, callsite_vec.begin(), callsite_vec.end());
  }
}
INSTANTIATE(DexMethod::gather_callsites, DexCallSite*)

template <typename C>
void DexMethod::gather_methodhandles(C& lmethodhandle) const {
  // We handle m_spec.cls and proto in the first-layer gather.
  std::vector<DexMethodHandle*> mhandles_vec; // Simplify refactor.
  if (m_code) m_code->gather_methodhandles(mhandles_vec);
  c_append_all(lmethodhandle, mhandles_vec.begin(), mhandles_vec.end());
}
INSTANTIATE(DexMethod::gather_methodhandles, DexMethodHandle*)
template <typename C>
void DexMethod::gather_strings_internal(C& lstring, bool exclude_loads) const {
  // We handle m_name and proto in the first-layer gather.
  std::vector<const DexString*> strings_vec; // Simplify refactor.
  if (m_code && !exclude_loads) m_code->gather_strings(strings_vec);
  if (m_anno) m_anno->gather_strings(strings_vec);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto& pair : *param_anno) {
      auto& anno_set = pair.second;
      anno_set->gather_strings(strings_vec);
    }
  }
  c_append_all(lstring, strings_vec.begin(), strings_vec.end());
}
void DexMethod::gather_strings(std::vector<const DexString*>& lstring,
                               bool exclude_loads) const {
  gather_strings_internal(lstring, exclude_loads);
}
void DexMethod::gather_strings(std::unordered_set<const DexString*>& lstring,
                               bool exclude_loads) const {
  gather_strings_internal(lstring, exclude_loads);
}

template <typename C>
void DexMethod::gather_fields(C& lfield) const {
  std::vector<DexFieldRef*> fields_vec; // Simplify refactor.
  if (m_code) m_code->gather_fields(fields_vec);
  if (m_anno) m_anno->gather_fields(fields_vec);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto& pair : *param_anno) {
      auto& anno_set = pair.second;
      anno_set->gather_fields(fields_vec);
    }
  }
  c_append_all(lfield, fields_vec.begin(), fields_vec.end());
}
INSTANTIATE(DexMethod::gather_fields, DexFieldRef*)

template <typename C>
void DexMethod::gather_methods(C& lmethod) const {
  if (m_code) {
    std::vector<DexMethodRef*> method_vec; // Simplify refactor.
    m_code->gather_methods(method_vec);
    c_append_all(lmethod, method_vec.begin(), method_vec.end());
  }
  gather_methods_from_annos(lmethod);
}
INSTANTIATE(DexMethod::gather_methods, DexMethodRef*)

template <typename C>
void DexMethod::gather_methods_from_annos(C& lmethod) const {
  std::vector<DexMethodRef*> method_vec; // Simplify refactor.
  if (m_anno) m_anno->gather_methods(method_vec);
  auto param_anno = get_param_anno();
  if (param_anno) {
    for (auto& pair : *param_anno) {
      auto& anno_set = pair.second;
      anno_set->gather_methods(method_vec);
    }
  }
  c_append_all(lmethod, method_vec.begin(), method_vec.end());
}
INSTANTIATE(DexMethod::gather_methods_from_annos, DexMethodRef*)

const DexMethod* DexMethodRef::as_def() const {
  if (is_def()) {
    return static_cast<const DexMethod*>(this);
  } else {
    return nullptr;
  }
}

DexMethod* DexMethodRef::as_def() {
  if (is_def()) {
    return static_cast<DexMethod*>(this);
  } else {
    return nullptr;
  }
}

template <typename C>
void DexMethodRef::gather_types_shallow(C& ltype) const {
  ltype.insert(ltype.end(), m_spec.cls);
  m_spec.proto->gather_types(ltype);
}
INSTANTIATE(DexMethodRef::gather_types_shallow, DexType*)

void DexMethodRef::gather_strings_shallow(
    std::vector<const DexString*>& lstring) const {
  lstring.insert(lstring.end(), m_spec.name);
  m_spec.proto->gather_strings(lstring);
}
void DexMethodRef::gather_strings_shallow(
    std::unordered_set<const DexString*>& lstring) const {
  lstring.insert(lstring.end(), m_spec.name);
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

DexProto* DexType::get_non_overlapping_proto(const DexString* method_name,
                                             DexProto* orig_proto) {
  auto methodref_in_context =
      DexMethod::get_method(this, method_name, orig_proto);
  if (!methodref_in_context) {
    return orig_proto;
  }
  DexTypeList::ContainerType new_arg_list;
  auto rtype = orig_proto->get_rtype();
  for (auto t : *orig_proto->get_args()) {
    new_arg_list.push_back(t);
  }
  new_arg_list.push_back(type::_int());
  DexTypeList* new_args =
      DexTypeList::make_type_list(DexTypeList::ContainerType{new_arg_list});
  DexProto* new_proto = DexProto::make_proto(rtype, new_args);
  methodref_in_context = DexMethod::get_method(this, method_name, new_proto);
  while (methodref_in_context) {
    new_arg_list.push_back(type::_int());
    new_args =
        DexTypeList::make_type_list(DexTypeList::ContainerType{new_arg_list});
    new_proto = DexProto::make_proto(rtype, new_args);
    methodref_in_context = DexMethod::get_method(this, method_name, new_proto);
  }
  return new_proto;
}

void DexMethod::add_load_params(size_t num_add_loads) {
  IRCode* code = this->get_code();
  always_assert_log(code, "Method don't have IRCode\n");
  auto callee_params = code->get_param_instructions();
  size_t added_params = 0;
  while (added_params < num_add_loads) {
    ++added_params;
    auto temp = code->allocate_temp();
    IRInstruction* new_param_load = new IRInstruction(IOPCODE_LOAD_PARAM);
    new_param_load->set_dest(temp);
    code->insert_before(callee_params.end(), new_param_load);
  }
}

void gather_components(std::vector<const DexString*>& lstring,
                       std::vector<DexType*>& ltype,
                       std::vector<DexFieldRef*>& lfield,
                       std::vector<DexMethodRef*>& lmethod,
                       std::vector<DexCallSite*>& lcallsite,
                       std::vector<DexMethodHandle*>& lmethodhandle,
                       const DexClasses& classes,
                       bool exclude_loads) {
  // Gather references reachable from each class.
  std::unordered_set<const DexString*> strings;
  std::unordered_set<DexType*> types;
  std::unordered_set<DexFieldRef*> fields;
  std::unordered_set<DexMethodRef*> methods;
  std::unordered_set<DexCallSite*> callsites;
  std::unordered_set<DexMethodHandle*> methodhandles;
  // Inside a lambda to ensure only visibility of the sets.
  [&classes, &exclude_loads, &strings, &types, &fields, &methods, &callsites,
   &methodhandles]() {
    for (auto const& cls : classes) {
      cls->gather_strings(strings, exclude_loads);
      cls->gather_types(types);
      cls->gather_fields(fields);
      cls->gather_methods(methods);
      cls->gather_callsites(callsites);
      cls->gather_methodhandles(methodhandles);
    }

    // Gather types and strings needed for field and method refs.
    for (auto meth : methods) {
      meth->gather_types_shallow(types);
      meth->gather_strings_shallow(strings);
    }

    for (auto field : fields) {
      field->gather_types_shallow(types);
      field->gather_strings_shallow(strings);
    }

    // Gather strings needed for each type.
    for (auto type : types) {
      if (type) strings.insert(type->get_name());
    }
  }();

  lstring.insert(lstring.end(), strings.begin(), strings.end());
  ltype.insert(ltype.end(), types.begin(), types.end());
  lfield.insert(lfield.end(), fields.begin(), fields.end());
  lmethod.insert(lmethod.end(), methods.begin(), methods.end());
  lcallsite.insert(lcallsite.end(), callsites.begin(), callsites.end());
  lmethodhandle.insert(lmethodhandle.end(), methodhandles.begin(),
                       methodhandles.end());

  // This retains pre-set computation behavior.
  sort_unique(lstring);
  sort_unique(ltype);
  sort_unique(lfield);
  sort_unique(lmethod);
  sort_unique(lcallsite);
  sort_unique(lmethodhandle);
}

std::string DexField::self_show() const { return show(this); }
std::string DexMethod::self_show() const { return show(this); }
std::string DexClass::self_show() const { return show(m_self); }

void DexMethodRef::erase_method(DexMethodRef* mref) {
  g_redex->erase_method(mref);
  if (mref->is_def()) {
    auto m = mref->as_def();
    if (m->get_name() != m->get_deobfuscated_name_or_null()) {
      g_redex->erase_method(m->get_class(), m->get_deobfuscated_name_or_null(),
                            m->get_proto());
    }
  }
}
