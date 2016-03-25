/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexCommon.h"
#include <stdint.h>
#include <string>

std::string format_map(ddump_data* rd);
std::string format_annotation_item(ddump_data* rd, const uint8_t** _aitem);
std::string format_encoded_value(ddump_data* rd, const uint8_t** _aitem);
std::string format_method(ddump_data* rd, int idx);
