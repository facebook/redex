/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sstream>

#include "DexAccess.h"
#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "DexDefs.h"
#include "DexIdx.h"
#include "DexOutput.h"

void DexDebugOpcodeSetFile::gather_strings(
    std::vector<const DexString*>& lstring) const {
  if (m_str) lstring.push_back(m_str);
}

void DexDebugOpcodeSetFile::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  DexDebugInstruction::encode(dodx, encdata);
  uint32_t fidx = DEX_NO_INDEX;
  if (m_str) {
    fidx = dodx->stringidx(m_str);
  }
  encdata = write_uleb128p1(encdata, fidx);
}

void DexDebugOpcodeStartLocal::gather_strings(
    std::vector<const DexString*>& lstring) const {
  if (m_name) lstring.push_back(m_name);
  if (m_sig) lstring.push_back(m_sig);
}

void DexDebugOpcodeStartLocal::gather_types(
    std::vector<DexType*>& ltype) const {
  if (m_type) ltype.push_back(m_type);
}

void DexDebugOpcodeStartLocal::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  DexDebugInstruction::encode(dodx, encdata);
  uint32_t nidx = DEX_NO_INDEX;
  uint32_t tidx = DEX_NO_INDEX;
  if (m_name) {
    nidx = dodx->stringidx(m_name);
  }
  if (m_type) {
    tidx = dodx->typeidx(m_type);
  }
  encdata = write_uleb128p1(encdata, nidx);
  encdata = write_uleb128p1(encdata, tidx);
  if (m_sig) {
    encdata = write_uleb128p1(encdata, dodx->stringidx(m_sig));
  }
}

void DexDebugInstruction::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  *encdata++ = (uint8_t)m_opcode;
  if (m_signed) {
    encdata = write_sleb128(encdata, m_value);
    return;
  }
  if (m_uvalue == DEX_NO_INDEX) return;
  encdata = write_uleb128(encdata, m_uvalue);
}

DexDebugInstruction* DexDebugInstruction::make_instruction(
    DexIdx* idx, const uint8_t** encdata_ptr) {
  auto& encdata = *encdata_ptr;
  uint8_t opcode = *encdata++;
  switch (opcode) {
  case DBG_END_SEQUENCE:
    return nullptr;
  case DBG_ADVANCE_PC:
  case DBG_END_LOCAL:
  case DBG_RESTART_LOCAL: {
    uint32_t v = read_uleb128(&encdata);
    return new DexDebugInstruction((DexDebugItemOpcode)opcode, v);
  }
  case DBG_ADVANCE_LINE: {
    int32_t v = (uint32_t)read_sleb128(&encdata);
    return new DexDebugInstruction((DexDebugItemOpcode)opcode, v);
  }
  case DBG_START_LOCAL: {
    uint32_t rnum = read_uleb128(&encdata);
    auto name = decode_noindexable_string(idx, encdata);
    DexType* type = decode_noindexable_type(idx, encdata);
    return new DexDebugOpcodeStartLocal(rnum, name, type);
  }
  case DBG_START_LOCAL_EXTENDED: {
    uint32_t rnum = read_uleb128(&encdata);
    auto name = decode_noindexable_string(idx, encdata);
    DexType* type = decode_noindexable_type(idx, encdata);
    auto sig = decode_noindexable_string(idx, encdata);
    return new DexDebugOpcodeStartLocal(rnum, name, type, sig);
  }
  case DBG_SET_FILE: {
    auto str = decode_noindexable_string(idx, encdata);
    return new DexDebugOpcodeSetFile(str);
  }
  default:
    return new DexDebugInstruction((DexDebugItemOpcode)opcode);
  };
}

// Returns a DexDebugInstruction corresponding to emitting a line entry
// with the given address offset and line offset. Asserts if invalid arguments.
std::unique_ptr<DexDebugInstruction> DexDebugInstruction::create_line_entry(
    int8_t line, uint8_t addr) {
  // These are limits imposed by
  // https://source.android.com/devices/tech/dalvik/dex-format#opcodes
  always_assert(line >= -4 && line <= 10);
  always_assert(addr <= 17);
  // Below is correct because adjusted_opcode = (addr * 15) + (line + 4), so
  // line_offset = -4 + (adjusted_opcode % 15) = -4 + line + 4 = line
  // addr_offset = adjusted_opcode / 15 = addr * 15 / 15 = addr since line + 4
  // is bounded by 0 and 14 we know (line + 4) / 15 = 0
  uint8_t opcode = 0xa + (addr * 15) + (line + 4);
  return std::make_unique<DexDebugInstruction>(
      static_cast<DexDebugItemOpcode>(opcode));
}

bool DexDebugInstruction::operator==(const DexDebugInstruction& that) const {
  return m_opcode == that.m_opcode && m_signed == that.m_signed &&
         (m_signed ? m_value == that.m_value : m_uvalue == that.m_uvalue);
};
