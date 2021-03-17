/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexEncoding.h"

#include <sstream>
#include <stdexcept>

namespace dex_encoding {
namespace details {

void throw_invalid(const char* msg, uint32_t size) {
  std::ostringstream exception_message;
  exception_message << msg << size;
  throw std::invalid_argument(exception_message.str());
}

void throw_invalid(const char* msg) { throw std::invalid_argument(msg); }

} // namespace details
} // namespace dex_encoding
