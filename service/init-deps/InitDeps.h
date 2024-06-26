/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace init_deps {
/*
 * Foo.<clinit> may read some static fields from class Bar, in which case
 * Bar.<clinit> will be executed first by the VM to determine the values of
 * those fields.
 *
 * Similarly, to ensure that our analysis of Foo.<clinit> knows as much about
 * Bar's static fields as possible, we want to analyze Bar.<clinit> before
 * Foo.<clinit>, since Foo.<clinit> depends on it. As such, we do a topological
 * sort of the classes here based on these dependencies.
 *
 * Note that the class initialization graph is *not* guaranteed to be acyclic.
 * (JLS SE7 12.4.1 indicates that cycles are indeed allowed.) In that case,
 * this pass cannot safely optimize the static final constants.
 */
Scope reverse_tsort_by_clinit_deps(const Scope& scope, size_t& init_cycles);

/**
 * Similar to reverse_tsort_by_clinit_deps(...), but since we are currently
 * only dealing with instance field from class that only have one <init>
 * so stop when we are at a class that don't have exactly one constructor,
 * we are not dealing with them now so we won't have knowledge about their
 * instance field.
 */
Scope reverse_tsort_by_init_deps(const Scope& scope, size_t& possible_cycles);
} // namespace init_deps
