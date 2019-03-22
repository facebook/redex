/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>

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
  DexDebugInstruction(DexDebugItemOpcode op, uint32_t v = DEX_NO_INDEX)
      : Gatherable() {
    m_opcode = op;
    m_uvalue = v;
    m_signed = false;
  }

  DexDebugInstruction(DexDebugItemOpcode op, int32_t v) : Gatherable() {
    m_opcode = op;
    m_value = v;
    m_signed = true;
  }

 public:
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);
  static DexDebugInstruction* make_instruction(DexIdx* idx,
                                               const uint8_t** encdata_ptr);
  virtual std::unique_ptr<DexDebugInstruction> clone() const {
    return std::make_unique<DexDebugInstruction>(*this);
  }

  DexDebugItemOpcode opcode() const { return m_opcode; }

  uint32_t uvalue() const { return m_uvalue; }

  int32_t value() const { return m_value; }

  void set_opcode(DexDebugItemOpcode op) { m_opcode = op; }

  void set_uvalue(uint32_t uv) { m_uvalue = uv; }

  void set_value(int32_t v) { m_value = v; }
};

class DexDebugOpcodeSetFile : public DexDebugInstruction {
 private:
  DexString* m_str;

 public:
  DexDebugOpcodeSetFile(DexString* str) : DexDebugInstruction(DBG_SET_FILE) {
    m_str = str;
  }

  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;
  void gather_strings(std::vector<DexString*>& lstring) const override;

  std::unique_ptr<DexDebugInstruction> clone() const override {
    return std::make_unique<DexDebugOpcodeSetFile>(*this);
  }

  DexString* file() const { return m_str; }

  void set_file(DexString* file) { m_str = file; }
};

class DexDebugOpcodeStartLocal : public DexDebugInstruction {
 private:
  DexString* m_name;
  DexType* m_type;
  DexString* m_sig;

 public:
  DexDebugOpcodeStartLocal(uint32_t rnum,
                           DexString* name,
                           DexType* type,
                           DexString* sig = nullptr)
      : DexDebugInstruction(DBG_START_LOCAL, rnum) {
    m_name = name;
    m_type = type;
    m_sig = sig;
    if (sig) m_opcode = DBG_START_LOCAL_EXTENDED;
  }

  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;
  void gather_strings(std::vector<DexString*>& lstring) const override;
  void gather_types(std::vector<DexType*>& ltype) const override;

  std::unique_ptr<DexDebugInstruction> clone() const override {
    return std::make_unique<DexDebugOpcodeStartLocal>(*this);
  }

  DexString* name() const { return m_name; }

  DexType* type() const { return m_type; }

  DexString* sig() const { return m_sig; }
};
