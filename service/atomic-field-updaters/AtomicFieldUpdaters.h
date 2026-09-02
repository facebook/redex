/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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

// Descriptors, for the rare caller that needs the string itself.
extern const char* const REFERENCE_DESC;
extern const char* const INTEGER_DESC;
extern const char* const LONG_DESC;

// The type of the field a given flavor updates: Object, int or long.
DexType* value_type(Kind kind);

// The flavors present in the program, mapped to their kind. A flavor absent
// from the map is one no loaded dex references, so `empty()` means the program
// contains no updaters at all and callers can skip their work entirely.
UnorderedMap<const DexType*, Kind> present_kinds();

// The updater types present in the program, without their kinds.
UnorderedSet<const DexType*> present_types();

} // namespace atomic_field_updaters
