/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "MatchFlow.h"

namespace optimize_enums {

// Maps the static enum fields to their ordinal number.
//
// This is all or nothing per enum - if we know some of the ordinals
// in an enum but are missing others, we don't know enough about the
// enum to safely undo any switchmapping.
using EnumFieldToOrdinal = std::unordered_map<DexField*, size_t>;

// Lookup tables in generated classes map enum ordinals to the integers they
// are represented by in switch statements using that lookup table:
//
//   lookup[enum.ordinal()] = case;
//
// GeneratedSwitchCases represent the reverse mapping for a lookup table:
//
//   gsc[lookup][case] = enum
//
// with lookup and enum identified by their fields.
using GeneratedSwitchCasetoField = std::unordered_map<int64_t, DexField*>;
using GeneratedSwitchCases =
    std::unordered_map<const DexFieldRef*, GeneratedSwitchCasetoField>;

/**
 * The match flow structure for switch map comparisons.
 */
class OptimizeEnumsUnmapMatchFlow final {
 public:
  mf::flow_t flow;

  mf::location_t kase;
  mf::location_t lookup;
  mf::location_t ordinal;
  mf::location_t aget;

  // A switch is the primary way a lookup will be used, but some
  // switches will be transformed into a series of ifs instead.
  mf::location_t cmp_switch;
  mf::location_t cmp_if_src0;
  mf::location_t cmp_if_src1;

  explicit OptimizeEnumsUnmapMatchFlow(
      const GeneratedSwitchCases& generated_switch_cases);
};

/**
 * Undoes switchmap ordinal mapping.
 *
 * This simply holds the constructed match flow and maps. It is thread-safe.
 */
class OptimizeEnumsUnmap final {
 public:
  OptimizeEnumsUnmap(const EnumFieldToOrdinal& enum_field_to_ordinal,
                     const GeneratedSwitchCases& generated_switch_cases);

  void unmap_switchmaps(cfg::ControlFlowGraph& cfg) const;

 private:
  const OptimizeEnumsUnmapMatchFlow m_flow;
  const EnumFieldToOrdinal& m_enum_field_to_ordinal;
  const GeneratedSwitchCases& m_generated_switch_cases;
};

} // namespace optimize_enums
