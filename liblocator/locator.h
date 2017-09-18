/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once
#include <stdint.h>

namespace facebook {

//
// A dex locator is a (strnr, dexnr, clsnr) tuple that provides a
// class-location hint to the native Dalvik classloader; this
// information helps the native classloader find classes without
// consulting the odex class-location hash tables.
//
// The dex locator encoding format is little-endian base-94, with
// between zero and four bytes of payload; a terminating NUL follows
// the payload.  The encoding must form a valid NUL-terminated MUTF-8
// string.  The string must end with the NUL byte, so '\0' is
// unavailable for use in the encoding; we must also leave the high
// bit unset, since the high bit indicates the beginning a non-ASCII
// UTF-8 sequence.  (We don't want to use UTF-8 itself since we want
// to be able to decode backward.) These constraints leave us with 127
// distinct usable values per byte.
//
// Since we decode backward, starting at the terminating NUL, we need
// to know when to stop decoding.  We stop at the ULEB length prefix
// that precedes a locator string in the dex string table.  The ULEB
// prefix for short things like our locator strings is just the number
// of bytes in the string, so it's a number between 0 and 4 inclusive;
// we need to exclude these values from the encoding alphabet.
//
// After excluding these values, we're left with base-122.  We get
// base-94 by additionally considering that 122 is not that much
// bigger than 94, that there are 94 printable characters in ASCII
// ('~' through '!' inclusive), that most dex files have less than
// 12,000 classes, that in both both-94 and base-122 encoding, we
// encode to three bytes of payload most of the time, and that
// debugging is easier when strings contain printable characters.
//
// The maximum value we can encode is 94^4, 78074896, or slightly
// greater than 2^26.  Taking six bits for a dex number (to match
// DexStore's limit of 64 dex files), we're left with support for 2^20
// classes per dex file, which is enough for anyone.
//
// The dex store identifier (used for modules loaded after the secondary
// dexes are loaded) is used by taking an additional 16 bits, bringing
// the total bits to encode to 2^42. Using the 6 encoded bits per byte,
// this brings the maximum locator size to 7 bytes, but the common case
// of strnr == 0 will result in a 3-4 byte locator
//
// An alternate scheme is using base-64, with six bits per byte.
// This scheme would have the advantage of substituting a
// multiplication with a shift during decoding, but multiply on modern
// ARM is only four bytes, and a base-64 scheme would push the common-
// case encoding from three to four bytes of payload.
//
// We bias all dex numbers by one so that we can reserve tuples of the
// form (0, 0, X) as special values.  (0, 0, 0) means to search the system
// class loader.
//

class Locator {
  // Number of bits in the locator we reserve for store number
  constexpr static const uint32_t strnr_bits = 16;

  // Number of bits in the locator we reserve for dex number
  constexpr static const uint32_t dexnr_bits = 6;

  // Number of bits (lower bound) available for a class number
  constexpr static const uint32_t clsnr_bits = 20;

  // Size (in bits) of a locator
  constexpr static const uint64_t bits = strnr_bits + dexnr_bits + clsnr_bits;

  constexpr static const uint64_t dexmask = (1LL << dexnr_bits) - 1;
  constexpr static const uint64_t clsmask = ((1LL << (dexnr_bits + clsnr_bits)) - 1)
                                            & ~dexmask;
  constexpr static const uint64_t strmask = ((1LL << (strnr_bits + clsnr_bits + dexnr_bits)) - 1)
                                            & ~(dexmask | clsmask);

  constexpr static const unsigned base = 94;
  constexpr static const unsigned bias = '!';

 public:

  const unsigned strnr;
  const unsigned dexnr; // 0 == special
  const unsigned clsnr;

  static Locator make(uint32_t str, uint32_t dex, uint32_t cls);

  // Maximum length (including NUL) of a locator string.
  // Estimating six bits per byte is conservative enough.
  constexpr static const uint32_t encoded_max = (bits + 5) / 6 + 1;

  uint32_t encode(char buf[encoded_max]) noexcept;

  static inline Locator decodeBackward(const char* endpos) noexcept;

 private:
  Locator(uint32_t str, uint32_t dex, uint32_t cls)
      : strnr(str), dexnr(dex), clsnr(cls) {}
};

Locator
Locator::decodeBackward(const char* endpos) noexcept
{
  // N.B. Because we _encode_ little-endian, when we _decode_
  // backward, we decode big-endian.

  uint64_t value = 0;
  const uint8_t* pos = (uint8_t*) (endpos - 1);
  while (*pos >= bias) {
    value = value * base + (*pos-- - bias);
  }

  uint32_t dex = (value & dexmask);
  uint32_t cls = (value & clsmask) >> dexnr_bits;
  uint32_t str = (value & strmask) >> (clsnr_bits + dexnr_bits);
  return Locator(str, dex, cls);
}

}
