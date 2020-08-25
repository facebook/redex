/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iosfwd>
#include <string>

#include "ProguardConfiguration.h"

namespace keep_rules {
namespace proguard_parser {

void parse_file(const std::string& filename, ProguardConfiguration* pg_config);
void parse(std::istream& config,
           ProguardConfiguration* pg_config,
           const std::string& filename = "");

/*
 * Typically used to remove keep rules that we wish to apply only to optimizers
 * that run prior to invoking Redex (e.g. ProGuard or R8).
 */
void remove_blocklisted_rules(ProguardConfiguration* pg_config);

} // namespace proguard_parser
} // namespace keep_rules
