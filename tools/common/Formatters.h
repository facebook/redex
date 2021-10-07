/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexCommon.h"
#include <stdint.h>
#include <string>

std::string format_map(ddump_data* rd);
std::string format_annotation(ddump_data* rd, const uint8_t** _aitem);
std::string format_annotation_item(ddump_data* rd, const uint8_t** _aitem);
std::string format_encoded_value(ddump_data* rd, const uint8_t** _aitem);
std::string format_method(ddump_data* rd, int idx);
std::string format_callsite(ddump_data* rd, const uint8_t** _aitem);
std::string format_method_handle_type(MethodHandleType type);
