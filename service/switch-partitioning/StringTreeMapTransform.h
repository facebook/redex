/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "DexClass.h"
#include "StringSwitchTransform.h"

/*
 * Re-encodes a recovered string switch into a compact StringTreeMap lookup: it
 * replaces the hashCode/switch/equals/ordinal (or if-equals-chain) machinery
 * with a single
 *
 *   const-string <encoded trie payload>
 *   const 0                                  // the "not found" sentinel
 *   invoke-static <lookup>(subject, payload, 0)  // -> ordinal (0 == miss)
 *   switch(ordinal) -> case bodies           // goto (ordinal 0) -> default
 *
 * The payload is a char-trie mapping each case-label string to a small ordinal.
 * Newly assigned ordinals are sequential in cfg::Block order, and multiple
 * labels that branch to the SAME block share one ordinal (so the emitted switch
 * stays packed and small).
 *
 * SIZE tier; magnitude is the estimated dex bytes saved. Applies to both
 * HASH_SWITCH and EQUALS_CHAIN forms when:
 *   - a lookup method is configured (the transform is only constructed when it
 *     resolves in the app),
 *   - the origin block is cold (a block with no SourceBlocks counts as cold),
 *   - every case-label string is printable ASCII (the trie encoder requires
 *     chars >= 32, and a clean MUTF-8 <-> UTF-16 round-trip requires ASCII),
 *   - the case count (including the default) is >= min_cases,
 *   - the encoded payload fits in a single const-string (<= max payload) -- V1.
 *
 * Region constants consumed in a case body (extra_loads) are supported: apply()
 * copies them to the front of those bodies before excising the region.
 *
 * The lookup is the KEY-first searchMap(subject, data, notFound): it
 * dereferences the subject, preserving the original switch's
 * NPE-on-null-subject behavior (so the discarded hashCode null-guard can be
 * removed safely).
 */
class StringTreeMapTransform : public StringSwitchTransform {
 public:
  StringTreeMapTransform(DexMethodRef* lookup_method,
                         int64_t min_cases,
                         size_t max_payload_size)
      : m_lookup_method(lookup_method),
        m_min_cases(min_cases),
        m_max_payload_size(max_payload_size) {}

  std::string_view name() const override { return "string_tree_map"; }

  std::optional<TransformScore> evaluate(
      const StringSwitchCandidate& candidate) const override;

  void apply(const StringSwitchCandidate& candidate) const override;

  // One method ref (the shared lookup method invoke) per dex.
  RefBudget reserve_refs() const override {
    return {/*frefs=*/0, /*mrefs=*/1, /*trefs=*/0};
  }

 private:
  DexMethodRef* m_lookup_method;
  int64_t m_min_cases;
  size_t m_max_payload_size;
};
