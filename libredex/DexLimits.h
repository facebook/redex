/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// We cannot have more than 2 ^ 15 type refs in one dex.
// NOTE: This is required because of a bug found in Android up to 7.
const size_t kMaxTypeRefs = 1 << 15;

// Methods and fields have the full 16-bit space available
const size_t kMaxMethodRefs = 64 * 1024;
const size_t kMaxFieldRefs = 64 * 1024;
