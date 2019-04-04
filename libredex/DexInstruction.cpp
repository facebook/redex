/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexInstruction.h"

#include "Debug.h"
#include "DexIdx.h"
#include "DexOutput.h"
#include "Warning.h"

unsigned DexInstruction::count_from_opcode() const {
  static int args[] = {
      0, /* FMT_f00x   */
      0, /* FMT_f10x   */
      0, /* FMT_f12x   */
      0, /* FMT_f12x_2 */
      0, /* FMT_f11n   */
      0, /* FMT_f11x_d */
      0, /* FMT_f11x_s */
      0, /* FMT_f10t   */
      1, /* FMT_f20t   */
      0, /* FMT_f20bc  */
      1, /* FMT_f22x   */
      1, /* FMT_f21t   */
      1, /* FMT_f21s   */
      1, /* FMT_f21h   */
      0, /* FMT_f21c_d */
      0, /* FMT_f21c_s */
      1, /* FMT_f23x_d */
      1, /* FMT_f23x_s */
      1, /* FMT_f22b   */
      1, /* FMT_f22t   */
      1, /* FMT_f22s   */
      0, /* FMT_f22c_d */
      0, /* FMT_f22c_s */
      0, /* FMT_f22cs  */
      2, /* FMT_f30t   */
      2, /* FMT_f32x   */
      2, /* FMT_f31i   */
      2, /* FMT_f31t   */
      1, /* FMT_f31c   */
      1, /* FMT_f35c   */
      2, /* FMT_f35ms  */
      2, /* FMT_f35mi  */
      1, /* FMT_f3rc   */
      2, /* FMT_f3rms  */
      2, /* FMT_f3rmi  */
      4, /* FMT_f51l   */
      1, /* FMT_f41c_d */
      1, /* FMT_f41c_s */
      2, /* FMT_f52c_d */
      2, /* FMT_f52c_s */
      2, /* FMT_f5rc */
      2, /* FMT_f57c */
      0, /* FMT_fopcode   */
  };
  return args[dex_opcode::format(opcode())];
};

DexOpcode DexInstruction::opcode() const {
  auto opcode = m_opcode & 0xff;
  if (opcode == OPCODE_NOP) {
    // Get the full opcode for pseudo-ops.
    return static_cast<DexOpcode>(m_opcode);
  }
  return static_cast<DexOpcode>(opcode);
}

DexInstruction* DexInstruction::set_opcode(DexOpcode op) {
  if (op >= FOPCODE_PACKED_SWITCH) {
    m_opcode = op;
  } else {
    m_opcode = (m_opcode & 0xff00) | op;
  }
  return this;
}

unsigned DexInstruction::dests_size() const {
  return dex_opcode::dests_size(opcode());
}

unsigned DexInstruction::srcs_size() const {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f00x:
  case FMT_f10x:
  case FMT_f11n:
  case FMT_f11x_d:
  case FMT_f10t:
  case FMT_f20t:
  case FMT_f21s:
  case FMT_f21h:
  case FMT_f21c_d:
  case FMT_f30t:
  case FMT_f31i:
  case FMT_f31c:
  case FMT_f3rc:
  case FMT_f51l:
  case FMT_f5rc:
  case FMT_f41c_d:
  case FMT_fopcode:
    return 0;
  case FMT_f12x:
  case FMT_f11x_s:
  case FMT_f22x:
  case FMT_f21t:
  case FMT_f21c_s:
  case FMT_f22b:
  case FMT_f22s:
  case FMT_f22c_d:
  case FMT_f32x:
  case FMT_f31t:
  case FMT_f41c_s:
  case FMT_f52c_d:
    return 1;
  case FMT_f12x_2:
  case FMT_f23x_d:
  case FMT_f22t:
  case FMT_f22c_s:
  case FMT_f52c_s:
    return 2;
  case FMT_f23x_s:
    return 3;
  case FMT_f35c:
  case FMT_f57c:
    return arg_word_count();
  case FMT_f20bc:
  case FMT_f22cs:
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rms:
  case FMT_f3rmi:
  case FMT_iopcode:
    always_assert_log(false, "Unimplemented opcode `%s'", SHOW(this));
  }
  not_reached();
}

uint16_t DexInstruction::dest() const {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f12x:
  case FMT_f12x_2:
  case FMT_f11n:
  case FMT_f22s:
  case FMT_f22c_d:
  case FMT_f22cs:
    return (m_opcode >> 8) & 0xf;
  case FMT_f11x_d:
  case FMT_f22x:
  case FMT_f21s:
  case FMT_f21h:
  case FMT_f21c_d:
  case FMT_f23x_d:
  case FMT_f22b:
  case FMT_f31i:
  case FMT_f31c:
  case FMT_f51l:
    return (m_opcode >> 8) & 0xff;
  case FMT_f32x:
    return m_arg[0];
  case FMT_f41c_d:
  case FMT_f52c_d:
    return m_arg[0];
  default:
    // All other formats do not define a destination register.
    always_assert_log(false, "Unhandled opcode: %s", SHOW(opcode()));
  }
  not_reached();
}

DexInstruction* DexInstruction::set_dest(uint16_t vreg) {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f12x:
  case FMT_f12x_2:
  case FMT_f11n:
  case FMT_f22s:
  case FMT_f22c_d:
  case FMT_f22cs:
    redex_assert((vreg & 0xf) == vreg);
    m_opcode = (m_opcode & 0xf0ff) | (vreg << 8);
    return this;
  case FMT_f11x_d:
  case FMT_f22x:
  case FMT_f21s:
  case FMT_f21h:
  case FMT_f21c_d:
  case FMT_f23x_d:
  case FMT_f22b:
  case FMT_f31i:
  case FMT_f31c:
  case FMT_f51l:
    redex_assert((vreg & 0xff) == vreg);
    m_opcode = (m_opcode & 0x00ff) | (vreg << 8);
    return this;
  case FMT_f32x:
    m_arg[0] = vreg;
    return this;
  case FMT_f41c_d:
  case FMT_f52c_d:
    m_arg[0] = vreg;
    return this;
  default:
    // All other formats do not define a destination register.
    always_assert_log(false, "Unhandled opcode: %s", SHOW(this));
  }
  not_reached();
}

uint16_t DexInstruction::src(int i) const {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f11x_s:
  case FMT_f21t:
  case FMT_f21c_s:
  case FMT_f31t:
    redex_assert(i == 0);
    return (m_opcode >> 8) & 0xff;
  case FMT_f12x:
  case FMT_f22s:
  case FMT_f22c_d:
    redex_assert(i == 0);
    return (m_opcode >> 12) & 0xf;
  case FMT_f12x_2:
    redex_assert(i < 2);
    if (i == 0) return (m_opcode >> 8) & 0xf;
    return (m_opcode >> 12) & 0xf;
  case FMT_f22x:
  case FMT_f3rc:
    redex_assert(i == 0);
    return m_arg[0];
  case FMT_f23x_d:
    redex_assert(i < 2);
    if (i == 0) return m_arg[0] & 0xff;
    return (m_arg[0] >> 8) & 0xff;
  case FMT_f23x_s:
    redex_assert(i < 3);
    if (i == 0) return (m_opcode >> 8) & 0xff;
    if (i == 1) return m_arg[0] & 0xff;
    return (m_arg[0] >> 8) & 0xff;
  case FMT_f22b:
    redex_assert(i == 0);
    return m_arg[0] & 0xff;
  case FMT_f22t:
  case FMT_f22c_s:
    redex_assert(i < 2);
    if (i == 0) return (m_opcode >> 8) & 0xf;
    if (i == 1) return (m_opcode >> 12) & 0xf;
  case FMT_f32x:
    redex_assert(i == 0);
    return m_arg[1];
  case FMT_f35c:
    redex_assert(i < 5);
    switch (i) {
    case 0:
      return m_arg[0] & 0xf;
    case 1:
      return (m_arg[0] >> 4) & 0xf;
    case 2:
      return (m_arg[0] >> 8) & 0xf;
    case 3:
      return (m_arg[0] >> 12) & 0xf;
    case 4:
      return (m_opcode >> 8) & 0xf;
    }
  case FMT_f41c_s:
    redex_assert(i == 0);
    return m_arg[0];
  case FMT_f52c_d:
    redex_assert(i == 0);
    return m_arg[1];
  case FMT_f52c_s:
    redex_assert(i <= 1);
    return m_arg[i];
  case FMT_f5rc:
    redex_assert(i == 0);
    return m_arg[1];
  case FMT_f57c:
    redex_assert(i <= 6);
    switch(i) {
    case 0:
      return (m_arg[0] >> 4) & 0xf;
    case 1:
      return (m_arg[0] >> 8) & 0xf;
    case 2:
      return (m_arg[0] >> 12) & 0xf;
    case 3:
      return m_arg[1] & 0xf;
    case 4:
      return (m_arg[1] >> 4) & 0xf;
    case 5:
      return (m_arg[1] >> 8) & 0xf;
    case 6:
      return (m_arg[1] >> 12) & 0xf;
    }
  default:
    // All other formats do not define source registers.
    always_assert_log(false, "Unhandled opcode: %s", SHOW(this));
  }
  not_reached();
}

DexInstruction* DexInstruction::set_src(int i, uint16_t vreg) {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f11x_s:
  case FMT_f21t:
  case FMT_f21c_s:
  case FMT_f31t:
    redex_assert(i == 0);
    redex_assert((vreg & 0xff) == vreg);
    m_opcode = (m_opcode & 0x00ff) | (vreg << 8);
    return this;
  case FMT_f12x:
  case FMT_f22s:
  case FMT_f22c_d:
    redex_assert(i == 0);
    redex_assert((vreg & 0xf) == vreg);
    m_opcode = (m_opcode & 0x0fff) | (vreg << 12);
    return this;
  case FMT_f12x_2:
    redex_assert(i < 2);
    redex_assert((vreg & 0xf) == vreg);
    if (i == 0) {
      m_opcode = (m_opcode & 0xf0ff) | (vreg << 8);
    } else {
      m_opcode = (m_opcode & 0x0fff) | (vreg << 12);
    }
    return this;
  case FMT_f22x:
    redex_assert(i == 0);
    m_arg[0] = vreg;
    return this;
  case FMT_f23x_d:
    redex_assert(i < 2);
    redex_assert((vreg & 0xff) == vreg);
    if (i == 0) {
      m_arg[0] = (m_arg[0] & 0xff00) | vreg;
      return this;
    }
    m_arg[0] = (m_arg[0] & 0x00ff) | (vreg << 8);
    return this;
  case FMT_f23x_s:
    redex_assert(i < 3);
    redex_assert((vreg & 0xff) == vreg);
    if (i == 0) {
      m_opcode = (m_opcode & 0x00ff) | (vreg << 8);
    } else if (i == 1) {
      m_arg[0] = (m_arg[0] & 0xff00) | vreg;
    } else {
      m_arg[0] = (m_arg[0] & 0x00ff) | (vreg << 8);
    }
    return this;
  case FMT_f22b:
    redex_assert(i == 0);
    redex_assert((vreg & 0xff) == vreg);
    m_arg[0] = (m_arg[0] & 0xff00) | vreg;
    return this;
  case FMT_f22t:
  case FMT_f22c_s:
    redex_assert(i < 2);
    redex_assert((vreg & 0xf) == vreg);
    if (i == 0) {
      m_opcode = (m_opcode & 0xf0ff) | (vreg << 8);
    } else {
      m_opcode = (m_opcode & 0x0fff) | (vreg << 12);
    }
    return this;
  case FMT_f32x:
    redex_assert(i == 0);
    m_arg[1] = vreg;
    return this;
  case FMT_f35c:
    redex_assert(i < 5);
    redex_assert((vreg & 0xf) == vreg);
    switch (i) {
    case 0:
      m_arg[0] = (m_arg[0] & 0xfff0) | vreg;
      return this;
    case 1:
      m_arg[0] = (m_arg[0] & 0xff0f) | (vreg << 4);
      return this;
    case 2:
      m_arg[0] = (m_arg[0] & 0xf0ff) | (vreg << 8);
      return this;
    case 3:
      m_arg[0] = (m_arg[0] & 0x0fff) | (vreg << 12);
      return this;
    case 4:
      m_opcode = (m_opcode & 0xf0ff) | (vreg << 8);
      return this;
    }
  case FMT_f41c_s:
    redex_assert(i == 0);
    m_arg[0] = vreg;
    return this;
  case FMT_f52c_d:
    redex_assert(i == 0);
    m_arg[1] = vreg;
    return this;
  case FMT_f52c_s:
    redex_assert(i <= 1);
    m_arg[i] = vreg;
    return this;
  case FMT_f57c:
    redex_assert(i <= 6);
    redex_assert((vreg & 0xf) == vreg);
    switch (i) {
    case 0:
      m_arg[0] = (m_arg[0] & 0xff0f) | (vreg << 4);
      return this;
    case 1:
      m_arg[0] = (m_arg[0] & 0xf0ff) | (vreg << 8);
      return this;
    case 2:
      m_arg[0] = (m_arg[0] & 0x0fff) | (vreg << 12);
      return this;
    case 3:
      m_arg[1] = (m_arg[1] & 0xfff0) | vreg;
      return this;
    case 4:
      m_arg[0] = (m_arg[1] & 0xff0f) | (vreg << 4);
      return this;
    case 5:
      m_arg[0] = (m_arg[1] & 0xf0ff) | (vreg << 8);
      return this;
    case 6:
      m_arg[0] = (m_arg[1] & 0x0fff) | (vreg << 12);
      return this;
    }
  default:
    // All other formats do not define source registers.
    always_assert_log(false, "Unhandled opcode: %s", SHOW(this));
  }
  not_reached();
}

DexInstruction* DexInstruction::set_srcs(const std::vector<uint16_t>& vregs) {
  for (size_t i = 0; i < vregs.size(); ++i) {
    set_src(i, vregs[i]);
  }
  return this;
}

template <int Width>
int64_t signext(uint64_t uv) {
  int shift = 64 - Width;
  return int64_t(uint64_t(uv) << shift) >> shift;
}

int64_t DexInstruction::get_literal() const {
  redex_assert(dex_opcode::has_literal(opcode()));
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f11n:
    return signext<4>(m_opcode >> 12);
  case FMT_f21s:
    return signext<16>(m_arg[0]);
  case FMT_f21h:
    return signext<16>(m_arg[0])
           << (opcode() == DOPCODE_CONST_WIDE_HIGH16 ? 48 : 16);
  case FMT_f22b:
    return signext<8>(m_arg[0] >> 8);
  case FMT_f22s:
    return signext<16>(m_arg[0]);
  case FMT_f31i: {
    auto literal = uint32_t(m_arg[0]) | (uint32_t(m_arg[1]) << 16);
    return signext<32>(literal);
  }
  case FMT_f51l: {
    auto literal = uint64_t(m_arg[0]) | (uint64_t(m_arg[1]) << 16) |
                   (uint64_t(m_arg[2]) << 32) | (uint64_t(m_arg[3]) << 48);
    return signext<64>(literal);
  }
  default:
    redex_assert(false);
  }
  not_reached();
}

DexInstruction* DexInstruction::set_literal(int64_t literal) {
  redex_assert(dex_opcode::has_literal(opcode()));
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f11n:
    m_opcode = (m_opcode & 0xfff) | ((literal & 0xf) << 12);
    return this;
  case FMT_f21s:
    m_arg[0] = literal;
    return this;
  case FMT_f21h:
    m_arg[0] = literal >> (opcode() == DOPCODE_CONST_WIDE_HIGH16 ? 48 : 16);
    return this;
  case FMT_f22b:
    m_arg[0] = (m_arg[0] & 0xFF) | ((literal << 8) & 0xFF00);
    return this;
  case FMT_f22s:
    m_arg[0] = literal;
    return this;
  case FMT_f31i:
    m_arg[0] = literal & 0xffff;
    m_arg[1] = literal >> 16;
    return this;
  case FMT_f51l:
    m_arg[0] = literal;
    m_arg[1] = literal >> 16;
    m_arg[2] = literal >> 32;
    m_arg[3] = literal >> 48;
    return this;
  default:
    redex_assert(false);
  }
  not_reached();
}

int32_t DexInstruction::offset() const {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f10t:
    return (int32_t)signext<8>(m_opcode >> 8);
  case FMT_f20t:
  case FMT_f21t:
  case FMT_f22t:
    return (int32_t) signext<16>(m_arg[0]);
  case FMT_f30t:
  case FMT_f31t: {
    auto offset = uint32_t(m_arg[0]) | (uint32_t(m_arg[1]) << 16);
    return  (int32_t) signext<32>(offset);
  }
  default:
    redex_assert(false);
  }
  not_reached();
}

DexInstruction* DexInstruction::set_offset(int32_t offset) {
  auto format = dex_opcode::format(opcode());
  switch (format) {
  case FMT_f10t:
    always_assert_log((int32_t)(int8_t)(offset & 0xff) == offset,
                      "offset %d too large for %s",
                      offset,
                      SHOW(this));
    m_opcode = (m_opcode & 0xff) | ((offset & 0xff) << 8);
    return this;
  case FMT_f20t:
  case FMT_f21t:
  case FMT_f22t:
    always_assert_log((int32_t)(int16_t)(offset & 0xffff) == offset,
                      "offset %d too large for %s",
                      offset,
                      SHOW(this));
    m_arg[0] = offset;
    return this;
  case FMT_f30t:
  case FMT_f31t:
    m_arg[0] = offset;
    m_arg[1] = offset >> 16;
    return this;
  default:
    redex_assert(false);
  }
  not_reached();
}

uint16_t DexInstruction::range_base() const {
  auto format = dex_opcode::format(opcode());
  redex_assert(format == FMT_f3rc || format == FMT_f5rc);
  if (format == FMT_f5rc) {
    return m_arg[1];
  }
  return m_arg[0];
}

uint16_t DexInstruction::range_size() const {
  auto format = dex_opcode::format(opcode());
  redex_assert(format == FMT_f3rc || format == FMT_f5rc);
  if (format == FMT_f5rc) return m_arg[0];
  return (m_opcode >> 8) & 0xff;
}

DexInstruction* DexInstruction::set_range_base(uint16_t base) {
  auto format = dex_opcode::format(opcode());
  redex_assert(format == FMT_f3rc || format == FMT_f5rc);
  if (format == FMT_f5rc) {
    m_arg[1] = base;
  } else {
    m_arg[0] = base;
  }
  return this;
}

DexInstruction* DexInstruction::set_range_size(uint16_t size) {
  auto format = dex_opcode::format(opcode());
  redex_assert(format == FMT_f3rc || format == FMT_f5rc);
  if (format == FMT_f5rc) {
    m_arg[0] = size;
  } else {
    redex_assert(size == (size & 0xff));
    m_opcode = (m_opcode & 0xff) | (size << 8);
  }
  return this;
}

uint16_t DexInstruction::arg_word_count() const {
  auto format = dex_opcode::format(opcode());
  redex_assert(format == FMT_f35c || format == FMT_f57c);
  if (format == FMT_f57c) {
    return (m_arg[0]) & 0xf;
  }
  return (m_opcode >> 12) & 0xf;
}

DexInstruction* DexInstruction::set_arg_word_count(uint16_t count) {
  auto format = dex_opcode::format(opcode());
  redex_assert(format == FMT_f35c || format == FMT_f57c);
  redex_assert((count & 0xf) == count);
  if (format == FMT_f57c) {
    m_arg[0] = (m_arg[0] & 0xfff0) | count;
  } else {
    m_opcode = (m_opcode & 0x0fff) | (count << 12);
  }
  return this;
}

void DexInstruction::verify_encoding() const {
  auto test = m_count ? new DexInstruction(opcode()) : new DexInstruction(opcode(), 0);
  if (dests_size()) {
    test->set_dest(dest());
  }
  for (unsigned i = 0; i < srcs_size(); i++) {
    test->set_src(i, src(i));
  }
  auto op = opcode();
  if (dex_opcode::has_range(op)) {
    test->set_range_base(range_base());
    test->set_range_size(range_size());
  }
  if (dex_opcode::has_arg_word_count(opcode()))
    test->set_arg_word_count(arg_word_count());
  if (dex_opcode::has_literal(op)) test->set_literal(get_literal());
  if (dex_opcode::has_offset(op)) test->set_offset(offset());

  assert_log(m_opcode == test->m_opcode, "%x %x\n", m_opcode, test->m_opcode);
  for (unsigned i = 0; i < m_count; i++) {
    assert_log(m_arg[i] == test->m_arg[i],
               "(%x %x) (%x %x)",
               m_opcode,
               m_arg[i],
               test->m_opcode,
               test->m_arg[i]);
  }

  delete test;
}

void DexOpcodeString::gather_strings(std::vector<DexString*>& lstring) const {
  lstring.push_back(m_string);
}

uint16_t DexOpcodeString::size() const { return jumbo() ? 3 : 2; }

void DexOpcodeString::encode(DexOutputIdx* dodx, uint16_t*& insns) {
  encode_opcode(dodx, insns);
  uint32_t sidx = dodx->stringidx(m_string);
  uint16_t idx = (uint16_t)sidx;
  if (!jumbo()) {
    always_assert_log(sidx == idx,
                      "Attempt to encode jumbo string in non-jumbo opcode: %s",
                      m_string->c_str());
    *insns++ = idx;
    return;
  }
  if (sidx == idx) {
    opt_warn(NON_JUMBO_STRING, "%s\n", m_string->c_str());
  }
  *insns++ = idx;
  idx = sidx >> 16;
  *insns++ = idx;
}

uint16_t DexOpcodeType::size() const { return m_count + 2; }

void DexOpcodeType::gather_types(std::vector<DexType*>& ltype) const {
  ltype.push_back(m_type);
}

void DexOpcodeType::encode(DexOutputIdx* dodx, uint16_t*& insns) {
  encode_opcode(dodx, insns);
  uint16_t idx = dodx->typeidx(m_type);
  *insns++ = idx;
  encode_args(insns);
}

void DexOpcodeField::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  lfield.push_back(m_field);
}

uint16_t DexOpcodeField::size() const { return 2; }

void DexOpcodeField::encode(DexOutputIdx* dodx, uint16_t*& insns) {
  encode_opcode(dodx, insns);
  uint16_t idx = dodx->fieldidx(m_field);
  *insns++ = idx;
}

void DexOpcodeMethod::gather_methods(
    std::vector<DexMethodRef*>& lmethod) const {
  lmethod.push_back(m_method);
}

uint16_t DexOpcodeMethod::size() const { return 3; }

void DexOpcodeMethod::encode(DexOutputIdx* dodx, uint16_t*& insns) {
  encode_opcode(dodx, insns);
  uint16_t idx = dodx->methodidx(m_method);
  *insns++ = idx;
  encode_args(insns);
}

uint16_t DexOpcodeData::size() const { return m_data_count + 1; }

void DexOpcodeData::encode(DexOutputIdx* dodx, uint16_t*& insns) {
  encode_opcode(dodx, insns);
  memcpy(insns, m_data, m_data_count * sizeof(uint16_t));
  insns += m_data_count;
}

void DexInstruction::encode(DexOutputIdx* dodx, uint16_t*& insns) {
  encode_opcode(dodx, insns);
  encode_args(insns);
}

uint16_t DexInstruction::size() const { return m_count + 1; }

DexInstruction* DexInstruction::make_instruction(DexIdx* idx,
                                                 const uint16_t** insns_ptr) {
  auto& insns = *insns_ptr;
  auto fopcode = static_cast<DexOpcode>(*insns++);
  DexOpcode opcode = static_cast<DexOpcode>(fopcode & 0xff);
  switch (opcode) {
  case DOPCODE_NOP: {
    if (fopcode == FOPCODE_PACKED_SWITCH) {
      int count = (*insns--) * 2 + 4;
      insns += count;
      return new DexOpcodeData(insns - count, count - 1);
    } else if (fopcode == FOPCODE_SPARSE_SWITCH) {
      int count = (*insns--) * 4 + 2;
      insns += count;
      return new DexOpcodeData(insns - count, count - 1);
    } else if (fopcode == FOPCODE_FILLED_ARRAY) {
      uint16_t ewidth = *insns++;
      uint32_t size = *((uint32_t*)insns);
      int count = (ewidth * size + 1) / 2 + 4;
      insns += count - 2;
      return new DexOpcodeData(insns - count, count - 1);
    }
  }
  /* Format 10, fall through for NOP */
  case DOPCODE_MOVE:
  case DOPCODE_MOVE_WIDE:
  case DOPCODE_MOVE_OBJECT:
  case DOPCODE_MOVE_RESULT:
  case DOPCODE_MOVE_RESULT_WIDE:
  case DOPCODE_MOVE_RESULT_OBJECT:
  case DOPCODE_MOVE_EXCEPTION:
  case DOPCODE_RETURN_VOID:
  case DOPCODE_RETURN:
  case DOPCODE_RETURN_WIDE:
  case DOPCODE_RETURN_OBJECT:
  case DOPCODE_CONST_4:
  case DOPCODE_MONITOR_ENTER:
  case DOPCODE_MONITOR_EXIT:
  case DOPCODE_THROW:
  case DOPCODE_GOTO:
  case DOPCODE_NEG_INT:
  case DOPCODE_NOT_INT:
  case DOPCODE_NEG_LONG:
  case DOPCODE_NOT_LONG:
  case DOPCODE_NEG_FLOAT:
  case DOPCODE_NEG_DOUBLE:
  case DOPCODE_INT_TO_LONG:
  case DOPCODE_INT_TO_FLOAT:
  case DOPCODE_INT_TO_DOUBLE:
  case DOPCODE_LONG_TO_INT:
  case DOPCODE_LONG_TO_FLOAT:
  case DOPCODE_LONG_TO_DOUBLE:
  case DOPCODE_FLOAT_TO_INT:
  case DOPCODE_FLOAT_TO_LONG:
  case DOPCODE_FLOAT_TO_DOUBLE:
  case DOPCODE_DOUBLE_TO_INT:
  case DOPCODE_DOUBLE_TO_LONG:
  case DOPCODE_DOUBLE_TO_FLOAT:
  case DOPCODE_INT_TO_BYTE:
  case DOPCODE_INT_TO_CHAR:
  case DOPCODE_INT_TO_SHORT:
  case DOPCODE_ADD_INT_2ADDR:
  case DOPCODE_SUB_INT_2ADDR:
  case DOPCODE_MUL_INT_2ADDR:
  case DOPCODE_DIV_INT_2ADDR:
  case DOPCODE_REM_INT_2ADDR:
  case DOPCODE_AND_INT_2ADDR:
  case DOPCODE_OR_INT_2ADDR:
  case DOPCODE_XOR_INT_2ADDR:
  case DOPCODE_SHL_INT_2ADDR:
  case DOPCODE_SHR_INT_2ADDR:
  case DOPCODE_USHR_INT_2ADDR:
  case DOPCODE_ADD_LONG_2ADDR:
  case DOPCODE_SUB_LONG_2ADDR:
  case DOPCODE_MUL_LONG_2ADDR:
  case DOPCODE_DIV_LONG_2ADDR:
  case DOPCODE_REM_LONG_2ADDR:
  case DOPCODE_AND_LONG_2ADDR:
  case DOPCODE_OR_LONG_2ADDR:
  case DOPCODE_XOR_LONG_2ADDR:
  case DOPCODE_SHL_LONG_2ADDR:
  case DOPCODE_SHR_LONG_2ADDR:
  case DOPCODE_USHR_LONG_2ADDR:
  case DOPCODE_ADD_FLOAT_2ADDR:
  case DOPCODE_SUB_FLOAT_2ADDR:
  case DOPCODE_MUL_FLOAT_2ADDR:
  case DOPCODE_DIV_FLOAT_2ADDR:
  case DOPCODE_REM_FLOAT_2ADDR:
  case DOPCODE_ADD_DOUBLE_2ADDR:
  case DOPCODE_SUB_DOUBLE_2ADDR:
  case DOPCODE_MUL_DOUBLE_2ADDR:
  case DOPCODE_DIV_DOUBLE_2ADDR:
  case DOPCODE_REM_DOUBLE_2ADDR:
  case DOPCODE_ARRAY_LENGTH:
    return new DexInstruction(fopcode);
  /* Format 20 */
  case DOPCODE_MOVE_FROM16:
  case DOPCODE_MOVE_WIDE_FROM16:
  case DOPCODE_MOVE_OBJECT_FROM16:
  case DOPCODE_CONST_16:
  case DOPCODE_CONST_HIGH16:
  case DOPCODE_CONST_WIDE_16:
  case DOPCODE_CONST_WIDE_HIGH16:
  case DOPCODE_GOTO_16:
  case DOPCODE_CMPL_FLOAT:
  case DOPCODE_CMPG_FLOAT:
  case DOPCODE_CMPL_DOUBLE:
  case DOPCODE_CMPG_DOUBLE:
  case DOPCODE_CMP_LONG:
  case DOPCODE_IF_EQ:
  case DOPCODE_IF_NE:
  case DOPCODE_IF_LT:
  case DOPCODE_IF_GE:
  case DOPCODE_IF_GT:
  case DOPCODE_IF_LE:
  case DOPCODE_IF_EQZ:
  case DOPCODE_IF_NEZ:
  case DOPCODE_IF_LTZ:
  case DOPCODE_IF_GEZ:
  case DOPCODE_IF_GTZ:
  case DOPCODE_IF_LEZ:
  case DOPCODE_AGET:
  case DOPCODE_AGET_WIDE:
  case DOPCODE_AGET_OBJECT:
  case DOPCODE_AGET_BOOLEAN:
  case DOPCODE_AGET_BYTE:
  case DOPCODE_AGET_CHAR:
  case DOPCODE_AGET_SHORT:
  case DOPCODE_APUT:
  case DOPCODE_APUT_WIDE:
  case DOPCODE_APUT_OBJECT:
  case DOPCODE_APUT_BOOLEAN:
  case DOPCODE_APUT_BYTE:
  case DOPCODE_APUT_CHAR:
  case DOPCODE_APUT_SHORT:
  case DOPCODE_ADD_INT:
  case DOPCODE_SUB_INT:
  case DOPCODE_MUL_INT:
  case DOPCODE_DIV_INT:
  case DOPCODE_REM_INT:
  case DOPCODE_AND_INT:
  case DOPCODE_OR_INT:
  case DOPCODE_XOR_INT:
  case DOPCODE_SHL_INT:
  case DOPCODE_SHR_INT:
  case DOPCODE_USHR_INT:
  case DOPCODE_ADD_LONG:
  case DOPCODE_SUB_LONG:
  case DOPCODE_MUL_LONG:
  case DOPCODE_DIV_LONG:
  case DOPCODE_REM_LONG:
  case DOPCODE_AND_LONG:
  case DOPCODE_OR_LONG:
  case DOPCODE_XOR_LONG:
  case DOPCODE_SHL_LONG:
  case DOPCODE_SHR_LONG:
  case DOPCODE_USHR_LONG:
  case DOPCODE_ADD_FLOAT:
  case DOPCODE_SUB_FLOAT:
  case DOPCODE_MUL_FLOAT:
  case DOPCODE_DIV_FLOAT:
  case DOPCODE_REM_FLOAT:
  case DOPCODE_ADD_DOUBLE:
  case DOPCODE_SUB_DOUBLE:
  case DOPCODE_MUL_DOUBLE:
  case DOPCODE_DIV_DOUBLE:
  case DOPCODE_REM_DOUBLE:
  case DOPCODE_ADD_INT_LIT16:
  case DOPCODE_RSUB_INT:
  case DOPCODE_MUL_INT_LIT16:
  case DOPCODE_DIV_INT_LIT16:
  case DOPCODE_REM_INT_LIT16:
  case DOPCODE_AND_INT_LIT16:
  case DOPCODE_OR_INT_LIT16:
  case DOPCODE_XOR_INT_LIT16:
  case DOPCODE_ADD_INT_LIT8:
  case DOPCODE_RSUB_INT_LIT8:
  case DOPCODE_MUL_INT_LIT8:
  case DOPCODE_DIV_INT_LIT8:
  case DOPCODE_REM_INT_LIT8:
  case DOPCODE_AND_INT_LIT8:
  case DOPCODE_OR_INT_LIT8:
  case DOPCODE_XOR_INT_LIT8:
  case DOPCODE_SHL_INT_LIT8:
  case DOPCODE_SHR_INT_LIT8:
  case DOPCODE_USHR_INT_LIT8: {
    uint16_t arg = *insns++;
    return new DexInstruction(fopcode, arg);
  }

  /* Format 30 */
  case DOPCODE_MOVE_16:
  case DOPCODE_MOVE_WIDE_16:
  case DOPCODE_MOVE_OBJECT_16:
  case DOPCODE_CONST:
  case DOPCODE_CONST_WIDE_32:
  case DOPCODE_FILL_ARRAY_DATA:
  case DOPCODE_GOTO_32:
  case DOPCODE_PACKED_SWITCH:
  case DOPCODE_SPARSE_SWITCH: {
    insns += 2;
    return new DexInstruction(insns - 3, 2);
  }
  /* Format 50 */
  case DOPCODE_CONST_WIDE: {
    insns += 4;
    return new DexInstruction(insns - 5, 4);
  }
  /* Field ref: */
  case DOPCODE_IGET:
  case DOPCODE_IGET_WIDE:
  case DOPCODE_IGET_OBJECT:
  case DOPCODE_IGET_BOOLEAN:
  case DOPCODE_IGET_BYTE:
  case DOPCODE_IGET_CHAR:
  case DOPCODE_IGET_SHORT:
  case DOPCODE_IPUT:
  case DOPCODE_IPUT_WIDE:
  case DOPCODE_IPUT_OBJECT:
  case DOPCODE_IPUT_BOOLEAN:
  case DOPCODE_IPUT_BYTE:
  case DOPCODE_IPUT_CHAR:
  case DOPCODE_IPUT_SHORT:
  case DOPCODE_SGET:
  case DOPCODE_SGET_WIDE:
  case DOPCODE_SGET_OBJECT:
  case DOPCODE_SGET_BOOLEAN:
  case DOPCODE_SGET_BYTE:
  case DOPCODE_SGET_CHAR:
  case DOPCODE_SGET_SHORT:
  case DOPCODE_SPUT:
  case DOPCODE_SPUT_WIDE:
  case DOPCODE_SPUT_OBJECT:
  case DOPCODE_SPUT_BOOLEAN:
  case DOPCODE_SPUT_BYTE:
  case DOPCODE_SPUT_CHAR:
  case DOPCODE_SPUT_SHORT: {
    uint16_t fidx = *insns++;
    DexFieldRef* field = idx->get_fieldidx(fidx);
    return new DexOpcodeField(fopcode, field);
  }
  /* MethodRef: */
  case DOPCODE_INVOKE_VIRTUAL:
  case DOPCODE_INVOKE_SUPER:
  case DOPCODE_INVOKE_DIRECT:
  case DOPCODE_INVOKE_STATIC:
  case DOPCODE_INVOKE_INTERFACE:
  case DOPCODE_INVOKE_VIRTUAL_RANGE:
  case DOPCODE_INVOKE_SUPER_RANGE:
  case DOPCODE_INVOKE_DIRECT_RANGE:
  case DOPCODE_INVOKE_STATIC_RANGE:
  case DOPCODE_INVOKE_INTERFACE_RANGE: {
    uint16_t midx = *insns++;
    uint16_t arg = *insns++;
    DexMethodRef* meth = idx->get_methodidx(midx);
    return new DexOpcodeMethod(fopcode, meth, arg);
  }
  /* StringRef: */
  case DOPCODE_CONST_STRING: {
    uint16_t sidx = *insns++;
    DexString* str = idx->get_stringidx(sidx);
    return new DexOpcodeString(fopcode, str);
  }
  case DOPCODE_CONST_STRING_JUMBO: {
    uint32_t sidx = *insns++;
    sidx |= (*insns++) << 16;
    DexString* str = idx->get_stringidx(sidx);
    return new DexOpcodeString(fopcode, str);
  }
  case DOPCODE_CONST_CLASS:
  case DOPCODE_CHECK_CAST:
  case DOPCODE_INSTANCE_OF:
  case DOPCODE_NEW_INSTANCE:
  case DOPCODE_NEW_ARRAY: {
    uint16_t tidx = *insns++;
    DexType* type = idx->get_typeidx(tidx);
    return new DexOpcodeType(fopcode, type);
  }
  case DOPCODE_FILLED_NEW_ARRAY:
  case DOPCODE_FILLED_NEW_ARRAY_RANGE: {
    uint16_t tidx = *insns++;
    uint16_t arg = *insns++;
    DexType* type = idx->get_typeidx(tidx);
    return new DexOpcodeType(fopcode, type, arg);
  }
  default:
    fprintf(stderr, "Unknown opcode %02x\n", opcode);
    return nullptr;
  }
}

DexInstruction* DexInstruction::make_instruction(DexOpcode op) {
  switch (op) {
  /* Field ref: */
  case DOPCODE_IGET:
  case DOPCODE_IGET_WIDE:
  case DOPCODE_IGET_OBJECT:
  case DOPCODE_IGET_BOOLEAN:
  case DOPCODE_IGET_BYTE:
  case DOPCODE_IGET_CHAR:
  case DOPCODE_IGET_SHORT:
  case DOPCODE_IPUT:
  case DOPCODE_IPUT_WIDE:
  case DOPCODE_IPUT_OBJECT:
  case DOPCODE_IPUT_BOOLEAN:
  case DOPCODE_IPUT_BYTE:
  case DOPCODE_IPUT_CHAR:
  case DOPCODE_IPUT_SHORT:
  case DOPCODE_SGET:
  case DOPCODE_SGET_WIDE:
  case DOPCODE_SGET_OBJECT:
  case DOPCODE_SGET_BOOLEAN:
  case DOPCODE_SGET_BYTE:
  case DOPCODE_SGET_CHAR:
  case DOPCODE_SGET_SHORT:
  case DOPCODE_SPUT:
  case DOPCODE_SPUT_WIDE:
  case DOPCODE_SPUT_OBJECT:
  case DOPCODE_SPUT_BOOLEAN:
  case DOPCODE_SPUT_BYTE:
  case DOPCODE_SPUT_CHAR:
  case DOPCODE_SPUT_SHORT:
    return new DexOpcodeField(op, nullptr);
  /* MethodRef: */
  case DOPCODE_INVOKE_VIRTUAL:
  case DOPCODE_INVOKE_SUPER:
  case DOPCODE_INVOKE_DIRECT:
  case DOPCODE_INVOKE_STATIC:
  case DOPCODE_INVOKE_INTERFACE:
  case DOPCODE_INVOKE_VIRTUAL_RANGE:
  case DOPCODE_INVOKE_SUPER_RANGE:
  case DOPCODE_INVOKE_DIRECT_RANGE:
  case DOPCODE_INVOKE_STATIC_RANGE:
  case DOPCODE_INVOKE_INTERFACE_RANGE:
    return new DexOpcodeMethod(op, nullptr);
  /* StringRef: */
  case DOPCODE_CONST_STRING:
  case DOPCODE_CONST_STRING_JUMBO:
    return new DexOpcodeString(op, nullptr);
  case DOPCODE_CONST_CLASS:
  case DOPCODE_CHECK_CAST:
  case DOPCODE_INSTANCE_OF:
  case DOPCODE_NEW_INSTANCE:
  case DOPCODE_NEW_ARRAY:
  case DOPCODE_FILLED_NEW_ARRAY:
  case DOPCODE_FILLED_NEW_ARRAY_RANGE:
    return new DexOpcodeType(op, nullptr);
  default:
    return new DexInstruction(op);
  }
}

bool DexInstruction::operator==(const DexInstruction& that) const {
  if (m_ref_type != that.m_ref_type ||
      m_opcode != that.m_opcode ||
      m_count != that.m_count) {
    return false;
  }
  for (size_t i = 0; i < m_count; ++i) {
    if (m_arg[i] != that.m_arg[i]) {
      return false;
    }
  }
  switch (m_ref_type) {
  case REF_NONE:
    return true;
  case REF_STRING: {
    auto this_ = static_cast<const DexOpcodeString*>(this);
    auto that_ = static_cast<const DexOpcodeString*>(&that);
    return this_->get_string() == that_->get_string();
  }
  case REF_TYPE: {
    auto this_ = static_cast<const DexOpcodeType*>(this);
    auto that_ = static_cast<const DexOpcodeType*>(&that);
    return this_->get_type() == that_->get_type();
  }
  case REF_FIELD: {
    auto this_ = static_cast<const DexOpcodeField*>(this);
    auto that_ = static_cast<const DexOpcodeField*>(&that);
    return this_->get_field() == that_->get_field();
  }
  case REF_METHOD: {
    auto this_ = static_cast<const DexOpcodeMethod*>(this);
    auto that_ = static_cast<const DexOpcodeMethod*>(&that);
    return this_->get_method() == that_->get_method();
  }
  }
}
