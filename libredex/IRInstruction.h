/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexInstruction.h"

using bit_width_t = uint8_t;

class IRInstruction final {
 public:
  explicit IRInstruction(DexOpcode op);
  explicit IRInstruction(const DexInstruction* dex_insn);

  static IRInstruction* make(const DexInstruction*);
  DexInstruction* to_dex_instruction() const;

  void range_to_srcs();
  /*
   * Converts invoke/fill-array instructions into their /range equivalents
   * if necessary. Will throw if the conversion is necessary but the src
   * registers are not consecutive.
   */
  void srcs_to_range();
  /*
   * Ensures that wide registers only have their first register referenced
   * in the srcs list. This only affects invoke-* instructions.
   */
  void normalize_registers();
  /*
   * Ensures that wide registers have both registers in the pair referenced
   * in the srcs list.
   */
  void denormalize_registers();

  uint16_t size() const;
  bool operator==(const IRInstruction&) const;
  bool operator!=(const IRInstruction& that) const {
    return !(*this == that);
  }

  bool has_string() const {
    return opcode::ref(m_opcode) == opcode::Ref::String;
  }
  bool has_type() const { return opcode::ref(m_opcode) == opcode::Ref::Type; }
  bool has_field() const {
    return opcode::ref(m_opcode) == opcode::Ref::Field;
  }
  bool has_method() const {
    return opcode::ref(m_opcode) == opcode::Ref::Method;
  }

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

  DexString* get_string() const {
    always_assert(has_string());
    return m_string;
  }

  IRInstruction* set_string(DexString* str) {
    always_assert(has_string());
    m_string = str;
    return this;
  }

  DexType* get_type() const {
    always_assert(has_type());
    return m_type;
  }

  IRInstruction* set_type(DexType* type) {
    always_assert(has_type());
    m_type = type;
    return this;
  }

  DexField* get_field() const {
    always_assert(has_field());
    return m_field;
  }

  IRInstruction* set_field(DexField* field) {
    always_assert(has_field());
    m_field = field;
    return this;
  }

  DexMethod* get_method() const {
    always_assert(has_method());
    return m_method;
  }

  IRInstruction* set_method(DexMethod* method) {
    always_assert(has_method());
    m_method = method;
    return this;
  }

  bool has_data() const {
    return opcode::ref(m_opcode) == opcode::Ref::Data;
  }

  DexOpcodeData* get_data() const {
    always_assert(has_data());
    return m_data;
  }

  IRInstruction* set_data(DexOpcodeData* data) {
    always_assert(has_data());
    m_data = data;
    return this;
  }

  void gather_strings(std::vector<DexString*>& lstring) const {
    if (has_string()) {
      lstring.push_back(m_string);
    }
  }

  void gather_types(std::vector<DexType*>& ltype) const {
    if (has_type()) {
      ltype.push_back(m_type);
    }
  }

  void gather_fields(std::vector<DexField*>& lfield) const {
    if (has_field()) {
      lfield.push_back(m_field);
    }
  }

  void gather_methods(std::vector<DexMethod*>& lmethod) const {
    if (has_method()) {
      lmethod.push_back(m_method);
    }
  }

 private:
  DexOpcode m_opcode;
  std::vector<uint16_t> m_srcs;
  uint16_t m_dest {0};
  union {
    DexString* m_string {nullptr};
    DexType* m_type;
    DexField* m_field;
    DexMethod* m_method;
    DexOpcodeData* m_data;
  };

  uint64_t m_literal {0};
  int32_t m_offset {0};
  std::pair<uint16_t, uint16_t> m_range {0, 0};
};

/*
 * The number of bits required to encode the given value. I.e. the offset of
 * the most significant bit.
 */
bit_width_t required_bit_width(uint16_t v);

/*
 * Necessary condition for an instruction to be converted to /range form
 */
bool has_contiguous_srcs(const IRInstruction*);

/*
 * Whether instruction must be converted to /range form in order to encode it
 * as a DexInstruction
 */
bool needs_range_conversion(const IRInstruction*);
