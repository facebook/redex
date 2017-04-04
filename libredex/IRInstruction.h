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
  explicit IRInstruction(DexOpcode op);

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
  size_t dests_size() const { return opcode::dests_size(m_opcode); }
  size_t srcs_size() const { return m_srcs.size(); }

  /*
   * Information about operands.
   */
  bool src_is_wide(size_t i) const;
  bool dest_is_wide() const;
  bool is_wide() const {
    return src_is_wide(0) || src_is_wide(1) || dest_is_wide();
  }

  /*
   * Accessors for logical parts of the instruction.
   */
  DexOpcode opcode() const { return m_opcode; }
  uint16_t dest() const {
    always_assert(opcode::dests_size(m_opcode));
    return m_dest;
  }
  uint16_t src(size_t i) const { return m_srcs.at(i); }
  uint16_t arg_word_count() const { return m_srcs.size(); }
  uint16_t range_base() const {
    always_assert(opcode::has_range(m_opcode));
    return m_range.first;
  }
  uint16_t range_size() const {
    always_assert(opcode::has_range(m_opcode));
    return m_range.second;
  }
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
  IRInstruction* set_src(size_t i, uint16_t vreg) {
    m_srcs.at(i) = vreg;
    return this;
  }
  IRInstruction* set_range_base(uint16_t vreg) {
    always_assert(opcode::has_range(m_opcode));
    m_range.first = vreg;
    return this;
  }
  IRInstruction* set_range_size(uint16_t size) {
    always_assert(opcode::has_range(m_opcode));
    m_range.second = size;
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
  uint16_t m_dest {0};

  uint64_t m_literal {0};
  int32_t m_offset {0};
  std::pair<uint16_t, uint16_t> m_range {0, 0};
};

class IRStringInstruction : public IRInstruction {
 private:
  DexString* m_string;

 public:
  IRStringInstruction(DexOpcode op, DexString* str)
      : IRInstruction(op), m_string(str) {
    m_ref_type = REF_STRING;
  }
  explicit IRStringInstruction(const DexOpcodeString* insn)
      : IRInstruction(insn), m_string(insn->get_string()) {
    m_ref_type = REF_STRING;
  }

  virtual void gather_strings(std::vector<DexString*>& lstring) const override {
    lstring.push_back(m_string);
  }
  virtual IRStringInstruction* clone() const override {
    return new IRStringInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  DexString* get_string() const { return m_string; }

  bool jumbo() const { return opcode() == OPCODE_CONST_STRING_JUMBO; }

  void rewrite_string(DexString* str) { m_string = str; }
};

class IRTypeInstruction : public IRInstruction {
 private:
  DexType* m_type;

 public:
  IRTypeInstruction(DexOpcode op, DexType* ty)
      : IRInstruction(op), m_type(ty) {
    m_ref_type = REF_TYPE;
  }
  explicit IRTypeInstruction(const DexOpcodeType* insn)
      : IRInstruction(insn), m_type(insn->get_type()) {
    m_ref_type = REF_TYPE;
  }

  virtual void gather_types(std::vector<DexType*>& ltype) const override {
    ltype.push_back(m_type);
  }
  virtual IRTypeInstruction* clone() const override {
    return new IRTypeInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  DexType* get_type() const { return m_type; }

  void rewrite_type(DexType* type) { m_type = type; }
};

class IRFieldInstruction : public IRInstruction {
 private:
  DexField* m_field;

 public:
  IRFieldInstruction(DexOpcode op, DexField* field)
      : IRInstruction(op), m_field(field) {
    m_ref_type = REF_FIELD;
  }
  explicit IRFieldInstruction(const DexOpcodeField* insn)
      : IRInstruction(insn), m_field(insn->field()) {
    m_ref_type = REF_FIELD;
  }

  virtual void gather_fields(std::vector<DexField*>& lfield) const override {
    lfield.push_back(m_field);
  }
  virtual IRFieldInstruction* clone() const override {
    return new IRFieldInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  DexField* field() const { return m_field; }
  void rewrite_field(DexField* field) { m_field = field; }
};

class IRMethodInstruction : public IRInstruction {
 private:
  DexMethod* m_method;

 public:
  IRMethodInstruction(DexOpcode op, DexMethod* method)
      : IRInstruction(op), m_method(method) {
    m_ref_type = REF_METHOD;
  }
  explicit IRMethodInstruction(const DexOpcodeMethod* insn)
      : IRInstruction(insn), m_method(insn->get_method()) {
    m_ref_type = REF_METHOD;
  }

  virtual void gather_methods(std::vector<DexMethod*>& lmethod) const override {
    lmethod.push_back(m_method);
  }
  virtual IRMethodInstruction* clone() const override {
    return new IRMethodInstruction(*this);
  }
  virtual DexInstruction* to_dex_instruction() const override;

  DexMethod* get_method() const { return m_method; }

  void rewrite_method(DexMethod* method) { m_method = method; }
};
