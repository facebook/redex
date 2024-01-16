/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassReferencesCache.h"

#include <cinttypes>
#include <cmath>
#include <numeric>

#include "DexUtil.h"
#include "WorkQueue.h"

ClassReferences::ClassReferences(const DexClass* cls) {
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(types);
  cls->gather_strings(strings);
  cls->gather_init_classes(init_types);

  // remove duplicates to speed up actual sorting
  sort_unique(method_refs);
  sort_unique(field_refs);
  sort_unique(types);
  sort_unique(strings);
  sort_unique(init_types);

  // sort deterministically
  std::sort(method_refs.begin(), method_refs.end(), compare_dexmethods);
  std::sort(field_refs.begin(), field_refs.end(), compare_dexfields);
  std::sort(types.begin(), types.end(), compare_dextypes);
  std::sort(strings.begin(), strings.end(), compare_dexstrings);
  std::sort(init_types.begin(), init_types.end(), compare_dextypes);
}

ClassReferencesCache::ClassReferencesCache(
    const std::vector<DexClass*>& classes) {
  workqueue_run<DexClass*>(
      [&](DexClass* cls) {
        m_cache.emplace(cls, std::make_shared<const ClassReferences>(cls));
      },
      classes);
}

std::shared_ptr<const ClassReferences> ClassReferencesCache::get(
    const DexClass* cls) const {
  auto res = m_cache.get(cls, nullptr);
  if (res) {
    return res;
  }
  res = std::make_shared<const ClassReferences>(cls);
  if (m_cache.emplace(cls, res)) {
    return res;
  }
  res = m_cache.get(cls, nullptr);
  always_assert(res);
  return res;
}
