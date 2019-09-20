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

/*
 * The proguard-android-optimize.txt file that is bundled with the Android SDK
 * has a keep rule to prevent removal of all resource ID fields. This is likely
 * because ProGuard runs before aapt which can change the values of those
 * fields. Since this is no longer true in our case, this rule is redundant and
 * hampers our optimizations.
 *
 * This function looks for that exact rule and removes it. I chose to do this
 * instead of unmarking all resource IDs so that if a resource ID really needs
 * to be kept, the user can still keep it by writing a keep rule that does a
 * non-wildcard match.
 */
void remove_blanket_resource_keep(ProguardConfiguration* pg_config);

} // namespace proguard_parser
} // namespace redex
