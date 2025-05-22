/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Function definitions should stay here instead of in the header if the
// function is not important to inline. This includes functions that are not
// frequently called, or takes a long time to run so that function call overhead
// is negligible. Otherwise, put them in the header.

#include "SignedConstantDomain.h"

SignedConstantDomain& SignedConstantDomain::left_shift_bits_int(int32_t shift) {
  return left_shift_bits(shift, BIT_SHIFT_MASK_INT);
}
SignedConstantDomain& SignedConstantDomain::left_shift_bits_long(
    int32_t shift) {
  return left_shift_bits(shift, BIT_SHIFT_MASK_LONG);
}

SignedConstantDomain& SignedConstantDomain::unsigned_right_shift_bits_int(
    int32_t shift) {
  if (is_bottom()) return *this;

  shift &= BIT_SHIFT_MASK_INT;

  // set_determined_bits_erasing_bounds() does not reset existing bit states.
  // Set to top first to clear bit states.
  const uint64_t new_determined_zeros =
      ~((static_cast<uint32_t>(~get_determined_zero_bits())) >> shift);
  const uint64_t new_determined_ones =
      static_cast<uint32_t>(get_determined_one_bits()) >> shift;
  set_to_top();
  set_determined_bits_erasing_bounds(new_determined_zeros,
                                     new_determined_ones,
                                     /*bit32=*/true);
  return *this;
}
SignedConstantDomain& SignedConstantDomain::unsigned_right_shift_bits_long(
    int32_t shift) {
  if (is_bottom()) return *this;

  shift &= BIT_SHIFT_MASK_LONG;

  // set_determined_bits_erasing_bounds() does not reset existing bit states.
  // Set to top first to clear bit states.
  const uint64_t new_determined_zeros =
      ~((~get_determined_zero_bits()) >> shift);
  const uint64_t new_determined_ones = get_determined_one_bits() >> shift;
  set_to_top();
  set_determined_bits_erasing_bounds(new_determined_zeros,
                                     new_determined_ones,
                                     /*bit32=*/false);
  return *this;
}
SignedConstantDomain& SignedConstantDomain::signed_right_shift_bits_int(
    int32_t shift) {
  return signed_right_shift_bits(shift, BIT_SHIFT_MASK_INT);
}
SignedConstantDomain& SignedConstantDomain::signed_right_shift_bits_long(
    int32_t shift) {
  return signed_right_shift_bits(shift, BIT_SHIFT_MASK_LONG);
}

SignedConstantDomain& SignedConstantDomain::left_shift_bits(int32_t shift,
                                                            int32_t mask) {
  if (is_bottom()) return *this;

  shift &= mask;

  // The higher 32 bits must be cleaned up, otherwise int meet may lead to
  // unintended bottoms due to mismatch in the higher 32 bits.
  uint64_t new_determined_zeros = (~((~get_determined_zero_bits()) << shift));
  uint64_t new_determined_ones = (get_determined_one_bits() << shift);
  set_to_top();
  set_determined_bits_erasing_bounds(new_determined_zeros,
                                     new_determined_ones,
                                     /*bit32=*/mask == BIT_SHIFT_MASK_INT);
  return *this;
}

SignedConstantDomain& SignedConstantDomain::signed_right_shift_bits(
    int32_t shift, int32_t mask) {
  if (is_bottom()) return *this;

  shift &= mask;

  // set_determined_bits_erasing_bounds() does not reset existing bit states.
  // Set to top first to clear bit states.
  const uint64_t new_determined_zeros = static_cast<uint64_t>(
      static_cast<int64_t>(get_determined_zero_bits()) >> shift);
  const uint64_t new_determined_ones = static_cast<uint64_t>(
      static_cast<int64_t>(get_determined_one_bits()) >> shift);
  set_to_top();
  set_determined_bits_erasing_bounds(new_determined_zeros,
                                     new_determined_ones,
                                     /*bit32=*/mask == BIT_SHIFT_MASK_INT);

  // Can explore additional inference on bounds here.
  return *this;
}

std::ostream& operator<<(std::ostream& o, const SignedConstantDomain& scd) {
  if (scd.is_bottom()) return o << "_|_";
  if (scd.is_top()) return o << "T";

  const auto print_bounds =
      [](std::ostream& o, const SignedConstantDomain& scd) -> std::ostream& {
    const auto min = scd.min_element();
    const auto max = scd.max_element();
    if (min == std::numeric_limits<int64_t>::min() &&
        max == std::numeric_limits<int64_t>::max()) {
      return o << (scd.is_nez() ? "NEZ" : "TB");
    }

    if (min == std::numeric_limits<int64_t>::min()) {
      if (max == -1) return o << "LTZ";
      if (max == 0) return o << "LEZ";
    }
    if (max == std::numeric_limits<int64_t>::max()) {
      if (min == 1) return o << "GTZ";
      if (min == 0) return o << "GEZ";
    }

    auto append = [&o](int64_t v) -> std::ostream& {
      if (v == std::numeric_limits<int64_t>::min()) return o << "min";
      if (v == std::numeric_limits<int64_t>::max()) return o << "max";
      return o << v;
    };

    if (min == max) return append(min);
    o << "[";
    append(min);
    if (min < 0 && max > 0 && scd.is_nez()) {
      o << ",-1]U[1,";
    } else {
      o << ",";
    }
    append(max);
    return o << "]";
  };

  print_bounds(o, scd);

  const auto print_bitset =
      [](std::ostream& o, const SignedConstantDomain& scd) -> std::ostream& {
    const auto min_max = SignedConstantDomain::from_constants(
        {scd.min_element(), scd.max_element()});
    if (min_max.get_zero_bit_states() == scd.get_zero_bit_states() &&
        min_max.get_one_bit_states() == scd.get_one_bit_states()) {
      // No interesting bitset info.
      return o;
    }

    std::ios old_io_state(nullptr);
    old_io_state.copyfmt(o);
    o << "{" << std::hex << std::showbase << scd.get_zero_bit_states() << "/"
      << scd.get_one_bit_states() << "}";
    o.copyfmt(old_io_state);
    return o;
  };

  return print_bitset(o, scd);
}
