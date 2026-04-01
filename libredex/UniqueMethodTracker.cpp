/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UniqueMethodTracker.h"

#include "DexHasher.h"

std::pair<const DexMethod*, bool> UniqueMethodTracker::insert(
    const DexMethod* method) {
  if (method->get_code() == nullptr || !method->get_code()->cfg_built()) {
    return {nullptr, false};
  }
  return insert(method, hashing::DexMethodHasher(method).run().code_hash);
}

std::pair<const DexMethod*, bool> UniqueMethodTracker::insert(
    const DexMethod* method, size_t code_hash) {
  const auto [ptr, inserted] = m_unique_methods.insert(Key{code_hash, method});
  return {ptr->method, inserted};
}
