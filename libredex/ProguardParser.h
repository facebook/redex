/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
