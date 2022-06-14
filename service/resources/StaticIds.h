/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <string>

namespace resources {
// Read the simple text file for static resource ids. Lines of the following are
// expected:
// com.facebook.packagename:string/flerp = 0x7f0a0123
void read_static_ids_file(const std::string& path,
                          const std::function<void(const std::string& package,
                                                   const std::string& type,
                                                   const std::string& name,
                                                   uint32_t value)>& callback);
} // namespace resources
