/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <vector>

class DexClass;

namespace inline_enum_values {

enum class Result {
  // If it is not possible or safe to inline `$values()`
  kIneligible,
  // The inliner declined to inline the call.
  kInlineFailed,
  // `$values()` was inlined into `<clinit>` and removed.
  kChanged,
};

struct Stats {
  size_t enums{0};
  size_t changed{0};
  size_t ineligible{0};
  size_t inline_failed{0};
};

/*
 * javac 15+ (JDK-8241798) emits a synthetic `private static E[] $values()`
 * method that builds the `$VALUES` array, and `<clinit>` calls it. Older javac
 * built `$VALUES` inline in `<clinit>` with no extra method. The split costs
 * extra dex size (the `$values()` method plus the invoke/move-result/return
 * plumbing), which regresses app size for large enums.
 *
 * `run` restores the pre-JDK-15 shape: it inlines the single `$values()` call
 * back into `<clinit>` and deletes the now-dead `$values()` method.
 *
 * This transform lives in OptimizeEnumsPass, gated by the
 * `inline_enum_values` config flag, so it runs after RearrangeEnumClinitPass
 * (which bails when
 * `$values()` has not been inlined) but is expressible as a single pass.
 */

// Per-class transform. Exposed for testing.
Result run(DexClass* cls);

// Driver over every enum class in `scope`. Runs per-class in parallel.
Stats run(const std::vector<DexClass*>& scope);

} // namespace inline_enum_values
