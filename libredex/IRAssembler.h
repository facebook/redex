/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "IRCode.h"
#include "S_Expression.h"

namespace assembler {

std::unique_ptr<IRCode> ircode_from_s_expr(const s_expr&);

std::unique_ptr<IRCode> ircode_from_string(const std::string&);

} // assembler
