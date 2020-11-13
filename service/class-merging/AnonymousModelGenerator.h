/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

class DexClass;
using Scope = std::vector<DexClass*>;
class PassManager;

namespace class_merging {

struct ModelSpec;

void discover_mergeable_anonymous_classes(const Scope& scope,
                                          size_t min_implementors,
                                          ModelSpec* merging_spec,
                                          PassManager* mgr);

} // namespace class_merging
