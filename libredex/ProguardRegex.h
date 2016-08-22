/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <string>

namespace redex {
namespace proguard_parser {

std::string form_member_regex(std::string proguard_regex);
std::string form_type_regex(std::string proguard_regex);

} // namespace proguard_parser
} // namespace redex
