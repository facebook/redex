/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexInstruction.h"
#include "Show.h"

/*
 * Our IR is very similar to the Dalvik instruction set, but with a few tweaks
 * to make it easier to analyze and manipulate. Key differences are:
 *
 * 1. Registers of arbitrary size can be addressed. For example, neg-int is no
 *    longer limited to addressing registers < 16. The expectation is that the
 *    register allocator will sort things out.
 *
 * 2. 2addr opcodes do not exist in IROpcode. Not aliasing src and dest values
 *    simplifies analyses.
 *
 * 3. range instructions do not exist in IROpcode. invoke-* instructions in our
 *    IR are not constrained in their number of src operands.
 *
 * 4. invoke-* instructions no longer reference both halves of a wide register.
 *    I.e. our IR represents them like `invoke-static {v0} LFoo;.bar(J)V` even
 *    though the Dex format will represent that as
 *    `invoke-static {v0, v1} LFoo;.bar(J)V`. All other instructions in the Dex
 *    format only refer to the lower half of a wide pair, so this makes things
 *    uniform.
 *
 * 5. Any opcode that can both throw and write to a dest register is split into
 *    two separate pieces in our IR: one piece that may throw but does not
 *    write to a dest, and one move-result-pseudo instruction that writes to a
 *    dest but does not throw. This makes accurate liveness analysis easy.
 *    This is elaborated further below.
 *
 * 6. check-cast also has a move-result-pseudo suffix. check-cast has a side
 *    effect in the runtime verifier when the cast succeeds. The runtime
 *    verifier updates the type in the source register to its more specific
 *    type. As such, for many analyses, it is semantically equivalent to
 *    creating a new value. By representing the opcode in our IR as having a
 *    dest field via move-result-pseudo, these analyses can be simplified by
 *    not having to treat check-cast as a special case.
 *
 *    See this link for the relevant verifier code:
 *    androidxref.com/7.1.1_r6/xref/art/runtime/verifier/method_verifier.cc#2383
 *
 * 7. payload instructions no longer exist. fill-array-data-payload is attached
 *    directly to the fill-array-data instruction that references it.
 *    {packed, sparse}-switch-payloads are represented by MFLOW_TARGET entries
 *    in the IRCode instruction stream.
 *
 * Background behind move-result-pseudo
 * ====================================
 * Opcodes that write to a register (say v0) but may also throw are somewhat
 * tricky to handle. Our dataflow analyses must consider v0 to be written
 * only if the opcode does not end up throwing.
 *
 * For example, say we have the following Dex code:
 *
 *   sget-object v1 <some field of type LQux;>
 *   const v0 #0
 *   start try block
 *   iget-object v0 v1 LQux;.a:LFoo;
 *   return-void
 *   // end try block
 *
 *   // exception handler
 *   invoke-static {v0} LQux;.a(LFoo;)V
 *
 * If `iget-object` throws, it will not have written to v0, so the `const` is
 * necessary to ensure that v0 is always initialized when control flow reaches
 * B2. In other words, v0 must be live-out at `const v0 #0`.
 *
 * Prior to this diff, we dealt with this by putting any potentially throwing
 * opcodes in the subsequent basic block when converting from Dex to IR:
 *
 *   B0:
 *     sget-object v1 <some field of type LQux;>
 *     const v0 #0
 *   B1: <throws to B2> // v1 is live-in here
 *     iget-object v0 v1 LQux;.a:LFoo;
 *     return-void
 *   B2: <catches exceptions from B1>
 *     invoke-static {v0} LQux;.a(LFoo;)V
 *
 * This way, straightforward liveness analysis will consider v0 to be
 * live-out at `const`. Obviously, this is still somewhat inaccurate: we end
 * up considering v1 as live-in at B1 when it should really be dead. Being
 * conservative about liveness generally doesn't create wrong behavior, but
 * can result in poorer optimizations.
 *
 * With move-result-pseudo, the above example will be represented as follows:
 *
 *   B0:
 *     sget-object v1 <some field of type LQux;>
 *     const v0 #0
 *     iget-object v1 LQux;.a:LFoo;
 *   B1: <throws to B2> // no registers are live-in here
 *     move-result-pseudo-object v0
 *     return-void
 *   B2: <catches exceptions from B1>
 *     invoke-static {v0} LQux;.a(LFoo;)V
 */
class IRInstruction final {
 public:
  explicit IRInstruction(IROpcode op);

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

  /*
   * Estimates the number of 16-bit code units required to encode this
   * IRInstruction. Since the exact encoding is only determined during
   * instruction lowering, this is just an estimate.
   */
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
  bool has_literal() const {
    return opcode::ref(m_opcode) == opcode::Ref::Literal;
  }

  /*
   * Number of registers used.
   */
  size_t dests_size() const { return opcode_impl::dests_size(m_opcode); }

  size_t srcs_size() const { return m_srcs.size(); }

  bool has_move_result_pseudo() const {
    return opcode_impl::has_move_result_pseudo(m_opcode);
  }

  bool has_move_result() const {
    return has_method() || has_move_result_pseudo() ||
           m_opcode == OPCODE_FILLED_NEW_ARRAY;
  }

  /*
   * Information about operands.
   */

  // Invoke instructions treat wide registers differently than *-wide
  // instructions. They explicitly refer to both halves of a pair, rather than
  // just the lower half. This method returns true on both lower and upper
  // halves.
  bool invoke_src_is_wide(size_t i) const;

  bool src_is_wide(size_t i) const;
  bool dest_is_wide() const {
    always_assert(dests_size());
    return opcode_impl::dest_is_wide(m_opcode);
  }
  bool is_wide() const {
    for (size_t i = 0; i < srcs_size(); i++) {
      if (src_is_wide(i)) {
        return true;
      }
    }
    return dests_size() && dest_is_wide();
  }

  /*
   * Accessors for logical parts of the instruction.
   */
  IROpcode opcode() const { return m_opcode; }
  uint16_t dest() const {
    always_assert_log(dests_size(), "No dest for %s", SHOW(m_opcode));
    return m_dest;
  }
  uint16_t src(size_t i) const { return m_srcs.at(i); }
  const std::vector<uint16_t>& srcs() const { return m_srcs; }
  uint16_t arg_word_count() const { return m_srcs.size(); }

  /*
   * Setters for logical parts of the instruction.
   */
  IRInstruction* set_opcode(IROpcode op) {
    m_opcode = op;
    return this;
  }
  IRInstruction* set_dest(uint16_t vreg) {
    always_assert(dests_size());
    m_dest = vreg;
    return this;
  }
  IRInstruction* set_src(size_t i, uint16_t vreg) {
    m_srcs.at(i) = vreg;
    return this;
  }
  IRInstruction* set_arg_word_count(uint16_t count) {
    m_srcs.resize(count);
    return this;
  }

  int64_t get_literal() const {
    always_assert(has_literal());
    return m_literal;
  }

  IRInstruction* set_literal(int64_t literal) {
    always_assert(has_literal());
    m_literal = literal;
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

  DexFieldRef* get_field() const {
    always_assert(has_field());
    return m_field;
  }

  IRInstruction* set_field(DexFieldRef* field) {
    always_assert(has_field());
    m_field = field;
    return this;
  }

  DexMethodRef* get_method() const {
    always_assert(has_method());
    return m_method;
  }

  IRInstruction* set_method(DexMethodRef* method) {
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

  void gather_types(std::vector<DexType*>& ltype) const;

  void gather_fields(std::vector<DexFieldRef*>& lfield) const {
    if (has_field()) {
      lfield.push_back(m_field);
    }
  }

  void gather_methods(std::vector<DexMethodRef*>& lmethod) const {
    if (has_method()) {
      lmethod.push_back(m_method);
    }
  }

  // Compute current instruction's hash.
  uint64_t hash() const;

 private:
  IROpcode m_opcode;
  std::vector<uint16_t> m_srcs;
  uint16_t m_dest{0};
  union {
    // Zero-initialize this union with the uint64_t member instead of a
    // pointer-type member so that it works properly even on 32-bit machines
    uint64_t m_literal{0};
    DexString* m_string;
    DexType* m_type;
    DexFieldRef* m_field;
    DexMethodRef* m_method;
    DexOpcodeData* m_data;
  };
};

/*
 * The number of bits required to encode the given value. I.e. the offset of
 * the most significant bit.
 */
bit_width_t required_bit_width(uint16_t v);

inline uint16_t max_unsigned_value(bit_width_t bits) { return (1 << bits) - 1; }

/*
 * Necessary condition for an instruction to be converted to /range form
 */
bool has_contiguous_srcs(const IRInstruction*);

/*
 * Whether instruction must be converted to /range form in order to encode it
 * as a DexInstruction
 */
bool needs_range_conversion(const IRInstruction*);

DexOpcode convert_2to3addr(DexOpcode op);

DexOpcode convert_3to2addr(DexOpcode op);
