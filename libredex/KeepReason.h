/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <ostream>
#include <unordered_set>

#include "Debug.h"

class DexMethod;

namespace keep_rules {
struct KeepSpec;
} // namespace keep_rules

/*
 * This module enumerates and defines the various reasons why we consider
 * certain classes / methods / fields as roots of the reachability graph.
 */
namespace keep_reason {

enum KeepReasonType {
  KEEP_RULE,
  REDEX_CONFIG,
  REFLECTION,
  MANIFEST,
  XML,
  ANNO,
  SERIALIZABLE,
  NATIVE,
  UNKNOWN,
};

struct Reason {
  KeepReasonType type;
  union {
    const keep_rules::KeepSpec* keep_rule{nullptr};
    const DexMethod* method;
  };

  explicit Reason(KeepReasonType type) : type(type) {
    always_assert(type != KEEP_RULE && type != REFLECTION);
  }

  explicit Reason(const keep_rules::KeepSpec* keep_rule)
      : type(KEEP_RULE), keep_rule(keep_rule) {}

  explicit Reason(KeepReasonType type, const DexMethod* reflection_source)
      : type(type), method(reflection_source) {
    // Right now, we only have one KeepReasonType that pairs with a DexMethod,
    // but we may have more in the future.
    always_assert(type == REFLECTION);
  }

  friend bool operator==(const Reason&, const Reason&);

  friend std::ostream& operator<<(std::ostream&, const Reason&);
};

size_t hash_value(const Reason&);

struct ReasonPtrHash {
  size_t operator()(const Reason* reason) const {
    return boost::hash<Reason>()(*reason);
  }
};

struct ReasonPtrEqual {
  size_t operator()(const keep_reason::Reason* r1,
                    const keep_reason::Reason* r2) const {
    return *r1 == *r2;
  }
};

using ReasonPtrSet =
    std::unordered_set<const Reason*, ReasonPtrHash, ReasonPtrEqual>;

} // namespace keep_reason
