/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <stdint.h>

namespace facebook {

//
// About global class indices
//
// A renamed class' type descriptor is of the form "LX/nnnnnn;" where nnnnnn is
// a base-62 encoding of its "global class index".
//
// The X helps our hacked classloader recognize that a
// class name is the output of the redex renamer and thus will
// never be found in the Android platform.
//
class Locator {
 public:
  // Number of bits in the locator we reserve for store number
  constexpr static const uint32_t strnr_bits = 16;

  // Number of bits in the locator we reserve for dex number
  constexpr static const uint32_t dexnr_bits = 6;

  // The obsolete name-based locator string format contained a special magic
  // locator.
  constexpr static const uint32_t magic_strnr = 27277;
  constexpr static const uint32_t magic_dexnr = 0;
  constexpr static const uint32_t magic_clsnr = 77227;

 private:
  // Number of bits (lower bound) available for a class number
  constexpr static const uint32_t clsnr_bits = 20;

  // Size (in bits) of a locator
  constexpr static const uint64_t bits = strnr_bits + dexnr_bits + clsnr_bits;

  constexpr static const uint64_t dexmask = (1LL << dexnr_bits) - 1;
  constexpr static const uint64_t clsmask =
      ((1LL << (dexnr_bits + clsnr_bits)) - 1) & ~dexmask;
  constexpr static const uint64_t strmask =
      ((1LL << (strnr_bits + clsnr_bits + dexnr_bits)) - 1) &
      ~(dexmask | clsmask);

  constexpr static const unsigned base = 94;
  constexpr static const unsigned bias = '!'; // 33

 public:
  const uint32_t strnr;
  const uint32_t dexnr; // 0 == special
  const uint32_t clsnr;

  static Locator make(uint32_t str, uint32_t dex, uint32_t cls);

  // Maximum length (including NUL) of a locator string.
  // Estimating six bits per byte is conservative enough.
  constexpr static const uint32_t encoded_max = (bits + 5) / 6 + 1;

  uint32_t encode(char buf[encoded_max]) noexcept;

  static inline Locator decodeBackward(const char* endpos) noexcept;

  // We use a base-62 encoding for global class indices.
  constexpr static const uint32_t global_class_index_digits_base = 62;
  // Encoded global class indices are of the form "LX/000000;" with at most
  // six digits.
  constexpr static const uint32_t global_class_index_digits_max = 6;
  constexpr static const uint32_t encoded_global_class_index_max =
      3 + global_class_index_digits_max + 1 + 1;
  static void encodeGlobalClassIndex(
      uint32_t globalClassIndex,
      size_t digits,
      char buf[encoded_global_class_index_max]) noexcept;
  constexpr static const uint32_t invalid_global_class_index = 0xFFFFFFFF;
  static inline uint32_t decodeGlobalClassIndex(
      const char* descriptor) noexcept;

  Locator(uint32_t str, uint32_t dex, uint32_t cls)
      : strnr(str), dexnr(dex), clsnr(cls) {}
};

Locator Locator::decodeBackward(const char* endpos) noexcept {
  // N.B. Because we _encode_ little-endian, when we _decode_
  // backward, we decode big-endian.

  uint64_t value = 0;
  const uint8_t* pos = (uint8_t*)(endpos - 1);
  while (*pos >= bias) {
    value = value * base + (*pos-- - bias);
  }

  uint32_t dex = (value & dexmask);
  uint32_t cls = (value & clsmask) >> dexnr_bits;
  uint32_t str = (value & strmask) >> (clsnr_bits + dexnr_bits);
  return Locator(str, dex, cls);
}

uint32_t Locator::decodeGlobalClassIndex(const char* descriptor) noexcept {
  // strip away array
  while (*descriptor == '[')
    ++descriptor;

  // remaining descriptor would have form "LX/nnnnnn;"
  if (descriptor[0] != 'L' || descriptor[1] != 'X' || descriptor[2] != '/') {
    return invalid_global_class_index;
  }
  descriptor += 3;

  uint64_t value = 0;
  char c = *(descriptor++);
  while (true) {
    if (c >= '0' && c <= '9') {
      value += c - '0';
    } else if (c >= 'A' && c <= 'Z') {
      value += c - 'A' + 10;
    } else if (c >= 'a' && c <= 'z') {
      value += c - 'a' + 10 + 26;
    } else {
      return invalid_global_class_index;
    }

    c = *(descriptor++);
    if (c == ';') {
      return *descriptor == '\0' ? value : invalid_global_class_index;
    }
    value *= 62;
  }
}

} // namespace facebook
