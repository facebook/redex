/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KeepReason.h"

#include <ostream>

#include "ConcurrentContainers.h"
#include "ProguardPrintConfiguration.h"
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

namespace {

// Lint will complain about this, but it is better than having to
// forward-declare all of concurrent containers.
std::unique_ptr<InsertOnlyConcurrentSet<keep_reason::Reason*,
                                        keep_reason::ReasonPtrHash,
                                        keep_reason::ReasonPtrEqual>>
    s_keep_reasons{nullptr};

} // namespace

bool Reason::s_record_keep_reasons = false;

void Reason::set_record_keep_reasons(bool v) {
  s_record_keep_reasons = v;
  if (v && s_keep_reasons == nullptr) {
    s_keep_reasons = std::make_unique<InsertOnlyConcurrentSet<
        keep_reason::Reason*, keep_reason::ReasonPtrHash,
        keep_reason::ReasonPtrEqual>>();
  }
}

Reason* Reason::try_insert(std::unique_ptr<Reason> to_insert) {
  auto [reason_ptr, emplaced] = s_keep_reasons->insert(to_insert.get());
  if (emplaced) {
    return to_insert.release();
  }
  return const_cast<Reason*>(*reason_ptr);
}

void Reason::release_keep_reasons() { s_keep_reasons.reset(); }

} // namespace keep_reason
