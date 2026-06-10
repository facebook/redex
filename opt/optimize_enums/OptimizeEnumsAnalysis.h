/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "DexClass.h"

namespace optimize_enums {

namespace impl {

// Forward declaration.
class Analyzer;

} // namespace impl

class OptimizeEnumsAnalysis final {
 public:
  ~OptimizeEnumsAnalysis();

  explicit OptimizeEnumsAnalysis(
      const DexClass* enum_cls,
      const UnorderedMap<const DexMethod*, uint32_t>& ctor_to_arg_ordinal);

  // Returns true if all candidate enum-field ordinals were determined.
  // Returns false if the analysis had to bail because at least one sfield's
  // value was top at <clinit> exit (in which case any partial entries added
  // to enum_field_to_ordinal for this class are erased before returning).
  bool collect_ordinals(UnorderedMap<DexField*, size_t>& enum_field_to_ordinal);

  // True iff the analysis observed an invoke-direct to one of this enum's
  // constructors whose ordinal argument was not a known constant. This is
  // the most common reason collect_ordinals() returns false; tracked
  // separately so callers can attribute degradation.
  bool had_unknown_ordinal_arg() const;

 private:
  std::unique_ptr<impl::Analyzer> m_analyzer;
  const DexClass* m_cls;
};

} // namespace optimize_enums
