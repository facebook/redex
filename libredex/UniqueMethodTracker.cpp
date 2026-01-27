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
  const DexMethod* representative = nullptr;
  bool was_new = false;

  // The third parameter to the updater is true if the entry already existed,
  // false if it was newly created.
  m_groups.update(
      Key{code_hash, method},
      [method, &representative, &was_new](
          const Key& key, UnorderedSet<const DexMethod*>& group, bool existed) {
        was_new = !existed;
        representative = existed ? key.method : method;
        group.insert(method);
      });

  return {representative, was_new};
}
