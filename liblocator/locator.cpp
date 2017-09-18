/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
  uint64_t value = strnr << clsnr_bits;
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

}
