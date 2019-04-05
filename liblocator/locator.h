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

//
// About name-based locators and global class indices
//
// Generally, we use locator strings when we cannot deduce a class' location
// otherwise. Before resorting to locator strings, our class loaders will also
// try to first check if a class name is the result of a systematic renaming
// process done by Redex that also allows us to determine the locator
// information.
// A renamed class' type descriptor is of the form "LX/nnnnnn;" where nnnnnn is
// a base-62 encoding of its "global class index".
// (The X helps our hacked Dalvik classloader recognize that a
// class name is the output of the redex renamer and thus will
// never be found in the Android platform.)
// The classes in each dex have global class indices in a particular range
// that doesn't overlap with any other dex. Conceptually, non-renamed classes
// that interleave with renamed classes also occupy a position in the global
// class index space. As a result, if the know the first global class index of
// each dex, we can compute the store, dex and (local) class index from the
// global class index of a renamed class.
//
// Similar to how general class locator strings as stored just in front of type
// descriptors in the string table, the global class index range information
// for a dex is stored in the string table as well, as two locator strings
// immediately preceeding the (empty) string with index 0, in the form of
// the locator string for first class in the dex, and the locator string for
// the last class in the dex.
//
// At runtime, all this kicks in when the manifest.txt file contains
// the line
//   .emit_name_based_locator_strings
// This line is added by Redex when renaming for name-based locators is enabled.
// Then, our various class loaders would load the range information on startup,
// and for class load requests  always first check if a type descriptor
// represents a global class index, and then find the dex into whose range the
// index falls, and then derive the locator information as described.
// Only for classes that didn't get renamed, and for which the global class
// index cannot be determined, the old locator string scheme still applies.
//
// In the class loader, the name-based scheme brings with it a single
// locator-string read for each dex at start up time (to learn about dex index
// ranges), while also getting rid of the subsequent need to read locator
// strings from the string table for every single class load event for renamed
// classes.
//
// The X helps our hacked Dalvik classloader recognize that a
// class name is the output of the redex renamer and thus will
// never be found in the Android platform.
//
class Locator {
 public:
  // Number of bits in the locator we reserve for store number
  constexpr static const uint32_t strnr_bits = 16;

  // Number of bits in the locator we reserve for dex number
  constexpr static const uint32_t dexnr_bits = 6;

 private:
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
  constexpr static const unsigned bias = '!'; // 33

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

  // We use a base-62 encoding for global class indices.
  constexpr static const uint32_t global_class_index_digits_base = 62;
  // Encoded global class indices are of the form "LX/000000;" with at most
  // six digits.
  constexpr static const uint32_t global_class_index_digits_max = 6;
  constexpr static const uint32_t encoded_global_class_index_max = 3 + global_class_index_digits_max + 1 + 1;
  static void encodeGlobalClassIndex(
      uint32_t globalClassIndex, size_t digits, char buf[encoded_global_class_index_max]) noexcept;
  constexpr static const uint32_t invalid_global_class_index = 0xFFFFFFFF;
      static inline uint32_t decodeGlobalClassIndex(
          const char* descriptor) noexcept;

  Locator(uint32_t str, uint32_t dex, uint32_t cls)
      : strnr(str), dexnr(dex), clsnr(cls) {}
};

Locator
Locator::decodeBackward(const char* endpos) noexcept
{
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
