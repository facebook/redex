/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AtomicFieldUpdaters.h"

#include <string_view>

#include "Debug.h"
#include "TypeUtil.h"

namespace atomic_field_updaters {

const char* const REFERENCE_DESC =
    "Ljava/util/concurrent/atomic/AtomicReferenceFieldUpdater;";
const char* const INTEGER_DESC =
    "Ljava/util/concurrent/atomic/AtomicIntegerFieldUpdater;";
const char* const LONG_DESC =
    "Ljava/util/concurrent/atomic/AtomicLongFieldUpdater;";

const char* const UNSAFE_DESC = "Lsun/misc/Unsafe;";
const char* const SYNTH_HOLDER_DESC = "Lredex/AtomicFieldUpdaterUnsafe;";

std::array<Kind, 3> all_kinds() {
  return {Kind::REFERENCE, Kind::INTEGER, Kind::LONG};
}

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

const DexString* new_updater_name() {
  // `get_string`, not `make_string`: absence means the program cannot contain
  // a call, so interning it would only grow the string table.
  return DexString::get_string("newUpdater");
}

namespace {
// The operations a lowering can express directly.
const UnorderedSet<std::string_view> kModeled{"get",
                                              "set",
                                              "lazySet",
                                              "compareAndSet",
                                              "getAndSet",
                                              "weakCompareAndSet",
                                              "getAndAdd",
                                              "addAndGet",
                                              "getAndIncrement",
                                              "getAndDecrement",
                                              "incrementAndGet",
                                              "decrementAndGet"};

// Plus the functional forms, which are operations but take an operator.
const UnorderedSet<std::string_view> kFunctional{
    "getAndUpdate", "updateAndGet", "getAndAccumulate", "accumulateAndGet"};
} // namespace

bool is_operation_name(std::string_view name) {
  return kModeled.count(name) != 0u || kFunctional.count(name) != 0u;
}

bool is_modeled_operation(std::string_view name) {
  return kModeled.count(name) != 0u;
}

uint16_t field_name_arg_index(Kind kind) {
  return kind == Kind::REFERENCE ? 2 : 1;
}

bool has_value_check(Kind kind) { return kind == Kind::REFERENCE; }

bool is_wide(Kind kind) { return kind == Kind::LONG; }

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
