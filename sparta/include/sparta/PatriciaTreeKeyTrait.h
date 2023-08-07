/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace sparta {

// Trait for types used as a key to a patricia tree.
//
// To be used as a key to a patricia tree, a given type must be safely
// `reinterpret_cast`-able to an unsigned integer type. It must be trivially
// copyable and destructible.
//
// This trait is implemented for unsigned integer types and pointers below.
template <typename Key, class = void>
struct PatriciaTreeKeyTrait {
  // The unsigned integer type used to encode the key.
  // using IntegerType = ...;

  // Dummy assert that always fails to error on key types that didn't implement
  // the trait.
  static_assert(!(sizeof(Key) >= 0),
                "Key does not implement PatriciaTreeKeyTrait");
};

template <typename T>
struct PatriciaTreeKeyTrait<T, std::enable_if_t<std::is_unsigned_v<T>>> {
  using IntegerType = T;
};

template <typename T>
struct PatriciaTreeKeyTrait<T*> {
  using IntegerType = std::uintptr_t;
};

} // namespace sparta
