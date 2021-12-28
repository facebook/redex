/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KeepReason.h"

#include "ProguardPrintConfiguration.h"
#include "RedexContext.h"
#include "Show.h"

namespace keep_reason {

std::ostream& operator<<(std::ostream& os, const Reason& reason) {
  switch (reason.type) {
  case KEEP_RULE:
    return os << "KEEP: " << keep_rules::show_keep(*reason.keep_rule, false);
  case REFLECTION:
    return os << "REFL: " << show_deobfuscated(reason.method);
  case REDEX_CONFIG:
    return os << "REDEX_CONFIG";
  case MANIFEST:
    return os << "MANIFEST";
  case XML:
    return os << "XML";
  case ANNO:
    return os << "ANNO";
  case SERIALIZABLE:
    return os << "SERIALIZABLE";
  case NATIVE:
    return os << "NATIVE";
  case UNKNOWN:
    return os << "UNKNOWN";
  }
}

bool operator==(const Reason& r1, const Reason& r2) {
  return r1.type == r2.type && r1.keep_rule == r2.keep_rule;
}

size_t hash_value(const Reason& reason) {
  size_t seed{0};
  boost::hash_combine(seed, reason.type);
  boost::hash_combine(seed, reason.keep_rule);
  return seed;
}

} // namespace keep_reason
