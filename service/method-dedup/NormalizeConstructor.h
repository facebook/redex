/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <vector>

// clang-format off
/**
 * Simple constructor only initializes the instance fields with the arguments
 * and call the super constructor. Reorder the arguments of the simple
 * constructors by their associated instance fields order.
 *
 * Example:
 * void <init>(B b, A a, D d, C c) {
 *   this.f2 = b;
 *   this.f1 = a;
 *   this.f3 = c;
 *   super.<init>(this, d);
 * }
 *
 * is logically equal to
 *
 * void <init>(A a, B b, C c, D d) {
 *   this.f1 = a;
 *   this.f2 = b;
 *   this.f3 = c;
 *   super.<init>(this, d);
 * }
 *
 * We summarize the logic of simple constructors and use the information to
 * help dedup the constructors, it's especially useful
 * when we merge anonymous classees together and want dedup the constructors as
 * many as possible.
 */
// clang-format on

class DexClass;

namespace method_dedup {

/**
 * Return the estimated code size reduction from constructor deduplication.
 */
uint32_t estimate_deduplicatable_ctor_code_size(const DexClass* cls);

/**
 * Deduplicate non-root constructors for each class and fix all the callsites.
 */
uint32_t dedup_constructors(const std::vector<DexClass*>& classes,
                            const std::vector<DexClass*>& scope);
} // namespace method_dedup
