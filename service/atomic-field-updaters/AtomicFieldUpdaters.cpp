/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AtomicFieldUpdaters.h"

#include "Debug.h"
#include "TypeUtil.h"

namespace atomic_field_updaters {

const char* const REFERENCE_DESC =
    "Ljava/util/concurrent/atomic/AtomicReferenceFieldUpdater;";
const char* const INTEGER_DESC =
    "Ljava/util/concurrent/atomic/AtomicIntegerFieldUpdater;";
const char* const LONG_DESC =
    "Ljava/util/concurrent/atomic/AtomicLongFieldUpdater;";

const char* kind_name(Kind kind) {
  switch (kind) {
  case Kind::REFERENCE:
    return "reference";
  case Kind::INTEGER:
    return "integer";
  case Kind::LONG:
    return "long";
  }
  not_reached();
}

DexType* value_type(Kind kind) {
  switch (kind) {
  case Kind::REFERENCE:
    return type::java_lang_Object();
  case Kind::INTEGER:
    return type::_int();
  case Kind::LONG:
    return type::_long();
  }
  not_reached();
}

UnorderedMap<const DexType*, Kind> present_kinds() {
  UnorderedMap<const DexType*, Kind> result;
  for (auto [kind, desc] : {std::pair{Kind::REFERENCE, REFERENCE_DESC},
                            {Kind::INTEGER, INTEGER_DESC},
                            {Kind::LONG, LONG_DESC}}) {
    // `get_type`, not `make_type`: a flavor no dex mentions stays absent, so
    // callers can cheaply detect a program with no updaters at all.
    if (auto* t = DexType::get_type(desc)) {
      result.emplace(t, kind);
    }
  }
  return result;
}

UnorderedSet<const DexType*> present_types() {
  UnorderedSet<const DexType*> result;
  const auto kinds = present_kinds();
  for (auto&& [type, kind] : UnorderedIterable(kinds)) {
    result.insert(type);
  }
  return result;
}

} // namespace atomic_field_updaters
