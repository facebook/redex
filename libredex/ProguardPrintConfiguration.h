/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexUtil.h"
#include "ProguardConfiguration.h"

namespace redex {

void show_configuration(std::ostream& output,
                        const Scope& classes,
                        const ProguardConfiguration& config);

std::string show_keep(const KeepSpec& keep_rule, bool show_source = true);
}

// namespace redex
