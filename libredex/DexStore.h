/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdlib>
#include <cstring>
#include "DexClass.h"

class DexClasses;

class DexStore {
  std::vector<DexClasses> m_dexen;

 public:
  DexStore() {};
  DexStore(const DexStore&) = delete;
  DexStore(DexStore&&) = default;

  std::vector<DexClasses>& get_dexen();

  void add_classes(DexClasses classes);
};
