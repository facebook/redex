/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <string_view>

#include "DeterministicContainers.h"
#include "DexClass.h"

/*
 * The `java.util.concurrent.atomic.Atomic*FieldUpdater` family.
 *
 * There are exactly three: the JDK provides no boolean, double, byte or short
 * variant. Code wanting one uses the Integer flavor over an `int` field --
 * `kotlinx.atomicfu` compiles `atomic(false)` that way -- or the Long flavor
 * with `Double.doubleToLongBits`.
 *
 * The `Atomic*` classes that are *not* field updaters (`AtomicInteger`, the
 * `*Array` types, `LongAdder`) are a different shape: they own their storage
 * rather than pointing at another object's field, so they have no offset to
 * resolve and no reflective access check to elide.
 */
namespace atomic_field_updaters {

// The three flavors. They differ in two ways that matter to callers:
// `AtomicReferenceFieldUpdater.newUpdater` takes (Class, Class, String) while
// the Integer and Long variants take (Class, String), so the field-name
// argument sits at a different index; and only the reference flavor performs a
// `valueCheck`, since a primitive value needs no type test.
enum class Kind { REFERENCE, INTEGER, LONG };

const char* kind_name(Kind kind);

// Every flavor, for callers that must cover all of them -- reporting a metric
// per flavor, say. Prefer this to writing the list out, which goes stale
// silently: a `switch` with no default is caught by the compiler, a hardcoded
// list is not.
std::array<Kind, 3> all_kinds();

// Descriptors, for the rare caller that needs the string itself.
extern const char* const REFERENCE_DESC;
extern const char* const INTEGER_DESC;
extern const char* const LONG_DESC;

// The lowering target: `sun.misc.Unsafe`, and the class Redex synthesizes to
// hold the shared instance of it. Shared so that a test asserting on what the
// lowering emitted names the same type the lowering names -- a test spelling
// the descriptor itself would keep passing if the two drifted apart.
//
// Deliberately not in `WellKnownTypes`: membership there is not just a cached
// accessor, it opts a type into `IRTypeChecker`'s assignability checking, which
// is skipped for external types outside the set. Unsafe is hidden platform API
// whose class definition is usually absent, so that is not a property to claim
// for it.
extern const char* const UNSAFE_DESC;
extern const char* const SYNTH_HOLDER_DESC;

// The factory every recognizer matches on. Each flavor declares exactly one
// `newUpdater` overload, so the name identifies it uniquely once the receiver
// class is pinned to one of the descriptors above -- match on both, never on
// the name alone.
//
// Null when no loaded dex interns the string, which is to say no call to it can
// exist; a caller comparing against the result then matches nothing, which is
// the right answer.
const DexString* new_updater_name();

// Every operation the family exposes, the functional forms included. Sizing
// the opportunity means counting operations a lowering does not yet handle, so
// this is deliberately wider than `is_modeled_operation`. What it excludes is
// the methods every object inherits -- `toString`, `hashCode`, `equals` -- and
// counting one of those as an updater operation would be noise.
bool is_operation_name(std::string_view name);

// The subset a lowering can model. The functional forms are out: their trailing
// argument is a `UnaryOperator`/`BinaryOperator` rather than a value, so the
// value written is only known at runtime.
//
// Matched by name rather than by argument shape: for the reference flavor the
// value type is `Object`, so a shape test cannot tell `set(T, V)` from
// `equals(Object)`, and a zero-argument inherited method would pass vacuously
// and then be read as though its absent first argument were the holder.
bool is_modeled_operation(std::string_view name);

// Index of the field-name argument to `newUpdater`. The reference flavor's
// signature is (Class tclass, Class vclass, String fieldName); the Integer and
// Long flavors omit the value class, so the name moves up one position. A
// recognizer written for one flavor silently matches nothing for the others.
uint16_t field_name_arg_index(Kind kind);

// Does this flavor type-check the value it writes? Only the reference flavor
// does: `valueCheck` tests `vclass.isInstance(v)`, and a primitive value needs
// no such test. A lowering must discharge this obligation only where it holds.
bool has_value_check(Kind kind);

// Does a value of this flavor occupy a wide register pair? Only long does,
// which decides whether a lowering needs wide temporaries and
// `move-result-wide`.
bool is_wide(Kind kind);

// The type of the field a given flavor updates: Object, int or long.
DexType* value_type(Kind kind);

// The flavors present in the program, mapped to their kind. A flavor absent
// from the map is one no loaded dex references, so `empty()` means the program
// contains no updaters at all and callers can skip their work entirely.
UnorderedMap<const DexType*, Kind> present_kinds();

// The updater types present in the program, without their kinds.
UnorderedSet<const DexType*> present_types();

} // namespace atomic_field_updaters
