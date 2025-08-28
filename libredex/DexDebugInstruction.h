/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>
#include <string_view>

#include "DexDefs.h"
#include "Gatherable.h"
#include "Util.h"

class DexIdx;
class DexOutputIdx;
class DexString;
class DexType;

class DexDebugInstruction : public Gatherable {
 private:
  union {
    uint32_t m_uvalue;
    int32_t m_value;
  };
  bool m_signed;

 protected:
  DexDebugItemOpcode m_opcode;

 public:
  explicit DexDebugInstruction(DexDebugItemOpcode op, uint32_t v = DEX_NO_INDEX)
      : m_uvalue(v), m_signed(false), m_opcode(op) {}

  DexDebugInstruction(DexDebugItemOpcode op, int32_t v)
      : m_value(v), m_signed(true), m_opcode(op) {}

  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);
  static DexDebugInstruction* make_instruction(DexIdx* idx,
                                               std::string_view& encdata_ptr);
  virtual std::unique_ptr<DexDebugInstruction> clone() const {
    return std::make_unique<DexDebugInstruction>(*this);
  }
  static std::unique_ptr<DexDebugInstruction> create_line_entry(int8_t line,
                                                                uint8_t addr);

  DexDebugItemOpcode opcode() const { return m_opcode; }

  uint32_t uvalue() const { return m_uvalue; }

  int32_t value() const { return m_value; }

  void set_opcode(DexDebugItemOpcode op) { m_opcode = op; }

  void set_uvalue(uint32_t uv) { m_uvalue = uv; }

  void set_value(int32_t v) { m_value = v; }

  bool operator==(const DexDebugInstruction&) const;

  bool operator!=(const DexDebugInstruction& that) const {
    return !(*this == that);
  }
};

class DexDebugOpcodeSetFile : public DexDebugInstruction {
 private:
  const DexString* m_str;

 public:
  explicit DexDebugOpcodeSetFile(const DexString* str)
      : DexDebugInstruction(DBG_SET_FILE) {
    m_str = str;
  }

  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;
  void gather_strings(std::vector<const DexString*>& lstring) const override;

  std::unique_ptr<DexDebugInstruction> clone() const override {
    return std::make_unique<DexDebugOpcodeSetFile>(*this);
  }

  const DexString* file() const { return m_str; }

  void set_file(const DexString* file) { m_str = file; }
};

class DexDebugOpcodeStartLocal : public DexDebugInstruction {
 private:
  const DexString* m_name;
  DexType* m_type;
  const DexString* m_sig;

 public:
  DexDebugOpcodeStartLocal(uint32_t rnum,
                           const DexString* name,
                           DexType* type,
                           const DexString* sig = nullptr)
      : DexDebugInstruction(DBG_START_LOCAL, rnum) {
    m_name = name;
    m_type = type;
    m_sig = sig;
    if (sig) {
      m_opcode = DBG_START_LOCAL_EXTENDED;
    }
  }

  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;
  void gather_strings(std::vector<const DexString*>& lstring) const override;
  void gather_types(std::vector<DexType*>& ltype) const override;

  std::unique_ptr<DexDebugInstruction> clone() const override {
    return std::make_unique<DexDebugOpcodeStartLocal>(*this);
  }

  const DexString* name() const { return m_name; }

  DexType* type() const { return m_type; }

  const DexString* sig() const { return m_sig; }
};
