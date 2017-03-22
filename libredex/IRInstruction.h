/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexInstruction.h"

class IRInstruction : public Gatherable {
 public:
  // TODO: construct directly instead of going via DexInstruction
  explicit IRInstruction(DexOpcode op)
      : IRInstruction(new DexInstruction(op)) {}

  static IRInstruction* make(const DexInstruction*);
  virtual IRInstruction* clone() const { return new IRInstruction(*this); }
  virtual DexInstruction* to_dex_instruction() const;
  uint16_t size() const;
  bool operator==(const IRInstruction&) const;
  bool operator!=(const IRInstruction& that) const {
    return !(*this == that);
  }

  bool has_strings() const { return m_ref_type == REF_STRING; }
  bool has_types() const { return m_ref_type == REF_TYPE; }
  bool has_fields() const { return m_ref_type == REF_FIELD; }
  bool has_methods() const { return m_ref_type == REF_METHOD; }

  /*
   * Number of registers used.
   */
  unsigned dests_size() const { return m_dests_size; }
  unsigned srcs_size() const { return m_srcs.size(); }
  bool has_range() const { return (bool)m_range; }
  bool has_literal() const;
  bool has_offset() const;

  /*
   * Information about operands.
   */
  bool src_is_wide(int i) const;
  bool dest_is_wide() const;
  bool is_wide() const {
    return src_is_wide(0) || src_is_wide(1) || dest_is_wide();
  }

  /*
   * Accessors for logical parts of the instruction.
   */
  DexOpcode opcode() const { return m_opcode; }
  uint16_t dest() const { return m_dest; }
  uint16_t src(int i) const { return m_srcs.at(i); }
  uint16_t arg_word_count() const { return m_srcs.size(); }
  uint16_t range_base() const { return m_range->first; }
  uint16_t range_size() const { return m_range->second; }
  int64_t literal() const { return m_literal; }
  int32_t offset() const { return m_offset; }

  /*
   * Setters for logical parts of the instruction.
   */
  IRInstruction* set_opcode(DexOpcode op) {
    m_opcode = op;
    return this;
  }
  IRInstruction* set_dest(uint16_t vreg) {
    m_dest = vreg;
    return this;
  }
  IRInstruction* set_src(int i, uint16_t vreg) {
    m_srcs.at(i) = vreg;
    return this;
  }
  IRInstruction* set_range_base(uint16_t vreg) {
    m_range->first = vreg;
    return this;
  }
  IRInstruction* set_range_size(uint16_t size) {
    m_range->second = size;
    return this;
  }
  IRInstruction* set_arg_word_count(uint16_t count) {
    m_srcs.resize(count);
    return this;
  }
  IRInstruction* set_literal(int64_t literal) {
    m_literal = literal;
    return this;
  }
  IRInstruction* set_offset(int32_t offset) {
    m_offset = offset;
    return this;
  }

 protected:
  enum {
    REF_NONE,
    REF_STRING,
    REF_TYPE,
    REF_FIELD,
    REF_METHOD
  } m_ref_type{REF_NONE};

  // use make()
  explicit IRInstruction(const DexInstruction* dex_insn);

  // use clone() instead
  IRInstruction(const IRInstruction&) = default;
  IRInstruction& operator=(const IRInstruction&) = default;

  void set_dex_instruction_args(DexInstruction*) const;

 private:
  DexOpcode m_opcode;
  std::vector<uint16_t> m_srcs;

  uint8_t m_dests_size; // 0 or 1
  uint16_t m_dest {0};

  uint64_t m_literal {0};
  int32_t m_offset {0};
  boost::optional<std::pair<uint16_t, uint16_t>> m_range;
};

class IRStringInstruction : public IRInstruction {
 private:
  DexString* m_string;

 public:
  // TODO: construct directly
  IRStringInstruction(DexOpcode op, DexString* str)
      : IRStringInstruction(new DexOpcodeString(op, str)) {}

  virtual void gather_strings(std::vector<DexString*>& lstring) const override {
    lstring.push_back(m_string);
  }
  virtual IRStringInstruction* clone() const override {
    return new IRStringInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  explicit IRStringInstruction(const DexOpcodeString* insn)
      : IRInstruction(insn), m_string(insn->get_string()) {
    m_ref_type = REF_STRING;
  }

  DexString* get_string() const { return m_string; }

  bool jumbo() const { return opcode() == OPCODE_CONST_STRING_JUMBO; }

  void rewrite_string(DexString* str) { m_string = str; }
};

class IRTypeInstruction : public IRInstruction {
 private:
  DexType* m_type;

 public:
  // TODO: construct directly
  IRTypeInstruction(DexOpcode op, DexType* ty)
      : IRTypeInstruction(new DexOpcodeType(op, ty)) {}
  virtual void gather_types(std::vector<DexType*>& ltype) const override {
    ltype.push_back(m_type);
  }
  virtual IRTypeInstruction* clone() const override {
    return new IRTypeInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  explicit IRTypeInstruction(const DexOpcodeType* insn)
      : IRInstruction(insn), m_type(insn->get_type()) {
    m_ref_type = REF_TYPE;
  }

  DexType* get_type() const { return m_type; }

  void rewrite_type(DexType* type) { m_type = type; }
};

class IRFieldInstruction : public IRInstruction {
 private:
  DexField* m_field;

 public:
  // TODO: construct directly
  IRFieldInstruction(DexOpcode op, DexField* field)
      : IRFieldInstruction(new DexOpcodeField(op, field)) {}
  virtual void gather_fields(std::vector<DexField*>& lfield) const override {
    lfield.push_back(m_field);
  }
  virtual IRFieldInstruction* clone() const override {
    return new IRFieldInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  explicit IRFieldInstruction(const DexOpcodeField* insn)
      : IRInstruction(insn), m_field(insn->field()) {
    m_ref_type = REF_FIELD;
  }

  DexField* field() const { return m_field; }
  void rewrite_field(DexField* field) { m_field = field; }
};

class IRMethodInstruction : public IRInstruction {
 private:
  DexMethod* m_method;

 public:
  // TODO: construct directly
  IRMethodInstruction(DexOpcode op, DexMethod* method)
      : IRMethodInstruction(new DexOpcodeMethod(op, method)) {}
  virtual void gather_methods(std::vector<DexMethod*>& lmethod) const override {
    lmethod.push_back(m_method);
  }
  virtual IRMethodInstruction* clone() const override {
    return new IRMethodInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  explicit IRMethodInstruction(const DexOpcodeMethod* insn)
      : IRInstruction(insn), m_method(insn->get_method()) {
    m_ref_type = REF_METHOD;
  }

  DexMethod* get_method() const { return m_method; }

  void rewrite_method(DexMethod* method) { m_method = method; }
};
