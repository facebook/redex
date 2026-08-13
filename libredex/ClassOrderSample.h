/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <boost/functional/hash.hpp>

// Utilities for emitting a compact, segment-aware, sampled fingerprint of the
// class placement (the linearized order of classes across dexes) of the main
// APK, so that two builds' placements can be compared per dex-ordering regime.
//
// The dexes of an APK are not ordered by a single rule: the primary dex is
// special; a few coldstart dexes are ordered for performance by the betamap;
// the rest are ordered by cross-dex-ref-minimization, with a tail of
// likely-dead classes. A single global order conflates these regimes, so we
// bucket each class into a Segment and fingerprint each segment independently.
//
// Within a segment the fingerprint maps a class-name hash to the class's dense
// index in emission order (plus the class's dex), so a downstream tool can
// compute an order-similarity (Kendall tau) per segment and detect classes that
// migrated between segments or dexes. We hash the (build-stable) deobfuscated
// name rather than the class structure, because the structure changes
// build-over-build even for "the same" class.
//
// Sampling is stratified: the performance-critical segments (primary, betamap)
// are kept in full, the large cold segment is sampled down to ~cold_cap via a
// name-hash threshold (so both builds independently select the same subset),
// and the dead (likely-unreachable) tail is counted but not sampled -- its
// order carries no signal.
namespace class_order_sample {

// Which dex-ordering regime a class belongs to. Computed by classify() below
// from primitive facts the caller extracts from the class, so this header stays
// free of Redex IR dependencies and unit-testable in isolation.
enum class Segment { PRIMARY, BETAMAP, COLD, DYNAMICALLY_DEAD };

// Bucket a class into its dex-ordering regime from the three primitive facts
// that determine it. Precedence matters:
//  - A dynamically-dead (Halfnosis) class is DYNAMICALLY_DEAD regardless of
//    anything else: its order carries no signal, so it must not pollute the
//    cold segment even though it lives among the cross-dex-ref-minimized dexes.
//  - Betamap-ordering then takes precedence over dex 0, because betamap-ordered
//    (coldstart) classes overwhelmingly live in the primary dex and earliest
//    secondaries; classifying dex 0 first would steal them out of the betamap
//    region -- gutting the very signal this fingerprint measures.
//  - PRIMARY is therefore only the non-dead, non-betamap residue of dex 0.
//  - Everything else is COLD.
inline Segment classify(bool is_dynamically_dead,
                        bool is_betamap_ordered,
                        uint32_t dex_index) {
  if (is_dynamically_dead) {
    return Segment::DYNAMICALLY_DEAD;
  }
  if (is_betamap_ordered) {
    return Segment::BETAMAP;
  }
  if (dex_index == 0) {
    return Segment::PRIMARY;
  }
  return Segment::COLD;
}

// Stable, deterministic 32-bit hash of a class name. Reuses boost::hash (the
// same primitive DexHasher builds on, and the same "stable enough across
// builds" guarantee the emitted pass-hashes already rely on), folded down to 32
// bits.
inline uint32_t name_hash(std::string_view name) {
  uint64_t h = boost::hash_range(name.begin(), name.end());
  return static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32);
}

// Inclusive-exclusive upper bound on the 32-bit hash space for a segment's
// sample: a class is kept iff name_hash(name) < threshold(...). Assuming
// uniformly distributed hashes this keeps ~cap classes; when the segment has no
// more than cap classes, everything is kept (threshold covers the whole space).
inline uint64_t threshold(uint64_t num_classes, uint64_t cap) {
  const uint64_t k_space = uint64_t(1) << 32;
  if (num_classes <= cap) {
    return k_space;
  }
  // Widen the product: k_space * cap overflows uint64_t for caps above 2^32.
  // The result always fits in uint64_t here because num_classes > cap, so it is
  // strictly less than k_space.
  return static_cast<uint64_t>(static_cast<unsigned __int128>(k_space) * cap /
                               num_classes);
}

// One class input, in emission order: deobfuscated name, whether it is
// generated, its segment, and its dex index.
struct ClassEntry {
  std::string_view name;
  bool is_generated;
  Segment segment;
  uint32_t dex;
};

// A sampled class within a segment.
struct SampledClass {
  uint32_t name_hash;
  uint32_t seg_index; // dense rank within the segment, in emission order
  uint32_t dex;
  bool operator==(const SampledClass& other) const {
    return name_hash == other.name_hash && seg_index == other.seg_index &&
           dex == other.dex;
  }
};

struct SegmentSample {
  std::vector<SampledClass> sampled;
  // Kept (non-skipped) classes in this segment: the sampling population, over
  // which seg_index runs densely.
  uint32_t num_classes{0};
  // Sampled classes dropped because their hash collided with an already-sampled
  // class in this segment (first kept, rest counted here).
  uint32_t collisions{0};
};

struct Sample {
  SegmentSample primary;
  SegmentSample betamap;
  SegmentSample cold;
  // Likely-dead (Halfnosis) classes are counted but not sampled.
  uint32_t dead_num_classes{0};
};

// Classes without a build-stable identity are skipped entirely: they neither
// count towards any population nor consume a seg_index. A class is skipped when
// it is generated (outlined/merged/etc., whose names are not build-stable) or
// has no deobfuscated name at all.
inline bool is_skipped(std::string_view name, bool is_generated) {
  return is_generated || name.empty();
}

namespace detail {
inline SegmentSample* sampled_segment(Sample& s, Segment seg) {
  switch (seg) {
  case Segment::PRIMARY:
    return &s.primary;
  case Segment::BETAMAP:
    return &s.betamap;
  case Segment::COLD:
    return &s.cold;
  case Segment::DYNAMICALLY_DEAD:
    return nullptr;
  }
  return nullptr;
}
} // namespace detail

// Build the stratified, segment-aware sample from the classes in emission
// order. primary and betamap are kept in full; cold is sampled down to
// ~cold_cap; dead is counted only. Selection within each sampled segment is a
// pure function of the class name, so two builds independently pick the same
// subset.
inline Sample build(const std::vector<ClassEntry>& classes, uint64_t cold_cap) {
  Sample sample;
  for (const auto& c : classes) {
    if (is_skipped(c.name, c.is_generated)) {
      continue;
    }
    if (c.segment == Segment::DYNAMICALLY_DEAD) {
      sample.dead_num_classes++;
    } else {
      detail::sampled_segment(sample, c.segment)->num_classes++;
    }
  }

  const uint64_t whole_space = uint64_t(1) << 32;
  const uint64_t cold_threshold = threshold(sample.cold.num_classes, cold_cap);

  uint32_t primary_index = 0, betamap_index = 0, cold_index = 0;
  std::unordered_set<uint32_t> primary_seen, betamap_seen, cold_seen;
  for (const auto& c : classes) {
    if (is_skipped(c.name, c.is_generated) ||
        c.segment == Segment::DYNAMICALLY_DEAD) {
      continue;
    }
    SegmentSample* seg = detail::sampled_segment(sample, c.segment);
    uint32_t* index = nullptr;
    uint64_t t = whole_space;
    std::unordered_set<uint32_t>* seen = nullptr;
    switch (c.segment) {
    case Segment::PRIMARY:
      index = &primary_index;
      t = whole_space;
      seen = &primary_seen;
      break;
    case Segment::BETAMAP:
      index = &betamap_index;
      t = whole_space;
      seen = &betamap_seen;
      break;
    case Segment::COLD:
      index = &cold_index;
      t = cold_threshold;
      seen = &cold_seen;
      break;
    case Segment::DYNAMICALLY_DEAD:
      continue; // filtered above; kept for enum exhaustiveness
    }
    const uint32_t h = name_hash(c.name);
    if (static_cast<uint64_t>(h) < t) {
      if (seen->insert(h).second) {
        seg->sampled.push_back(SampledClass{h, *index, c.dex});
      } else {
        seg->collisions++;
      }
    }
    (*index)++;
  }
  return sample;
}

} // namespace class_order_sample
