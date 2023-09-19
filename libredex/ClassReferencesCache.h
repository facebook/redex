/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include "ConcurrentContainers.h"
#include "DexClass.h"

struct ClassReferences {
  explicit ClassReferences(const DexClass* cls);

  bool operator==(const ClassReferences& other) const;

  std::vector<DexMethodRef*> method_refs;
  std::vector<DexFieldRef*> field_refs;
  std::vector<DexType*> types;
  std::vector<const DexString*> strings;
  std::vector<DexType*> init_types;
};

class ClassReferencesCache {
 public:
  explicit ClassReferencesCache(const std::vector<DexClass*>& classes);
  const ClassReferences& get(const DexClass* cls) const;

 private:
  mutable InsertOnlyConcurrentMap<const DexClass*, ClassReferences> m_cache;
};
