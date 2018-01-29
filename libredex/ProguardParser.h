/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <memory>
#include <set>
#include <vector>

#include "ProguardConfiguration.h"
#include "ProguardLexer.h"

namespace redex {
namespace proguard_parser {

void parse_file(const std::string& filename, ProguardConfiguration* pg_config);
void parse(istream& config,
           ProguardConfiguration* pg_config,
           const std::string& filename = "");

} // namespace proguard_parser
} // namespace redex
