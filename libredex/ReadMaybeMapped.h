/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <string>

namespace redex {

void read_file_with_contents(const std::string& file,
                             const std::function<void(const char*, size_t)>& fn,
                             size_t threshold = 64 * 1024);

} // namespace redex
