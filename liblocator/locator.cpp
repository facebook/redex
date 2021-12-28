/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <exception>
#include <stdexcept>
#include <assert.h>
#include "locator.h"

namespace facebook {

Locator
Locator::make(uint32_t str, uint32_t dex, uint32_t cls)
{
  if (str >= (1 << strnr_bits)) {
    throw std::runtime_error("too many dex stores");
  }

  if (dex >= (1 << dexnr_bits)) {
    throw std::runtime_error("too many dex files");
  }

  if (cls >= (1 << clsnr_bits)) {
    throw std::runtime_error("too many classes in one dex");
  }

  return Locator(str, dex, cls);
}

uint32_t
Locator::encode(char buf[encoded_max]) noexcept
{
  uint64_t value = ((uint64_t)strnr) << clsnr_bits;
  value = (value | clsnr) << dexnr_bits;
  value = (value | dexnr);
  uint8_t* pos = (uint8_t*) &buf[0];
  while (value != 0) {
    uint8_t enc = (value % base) + bias;
    assert((enc & 0x80) == 0);
    assert(enc >= bias);
    *pos++ = enc;
    value /= base;
  }
  *pos = '\0';
  uint32_t len = (pos - (uint8_t*) buf);
  assert(len <= encoded_max);
  return len;
}

static char getDigit(uint32_t num) {
  assert(num >= 0 && num < Locator::global_class_index_digits_base);
  if (num < 10) {
    return num + '0';
  } else if (num >= 10 && num < 36) {
    return num - 10 + 'A';
  } else {
    return num - 10 - 26 + 'a';
  }
}

void Locator::encodeGlobalClassIndex(
    uint32_t globalClassIndex, size_t digits, char buf[encoded_global_class_index_max]) noexcept
{
  assert(digits > 0 && digits <= global_class_index_digits_max);

  char* pos = buf;
  *pos++ = 'L';
  *pos++ = 'X';
  *pos++ = '/';

  uint32_t num = globalClassIndex;
  char* digit_pos = pos + digits;
  do {
    *--digit_pos = getDigit(num % global_class_index_digits_base);
    num /= global_class_index_digits_base;
  } while (digit_pos != pos);
  assert(num == 0);
  pos += digits;

  *pos++ = ';';
  *pos++ = '\0';

  assert(static_cast<uint32_t>(pos - buf) <= encoded_global_class_index_max);
}

}
