/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexStore.h"

std::vector<DexClasses>& DexStore::get_dexen() {
  return m_dexen;
}

void DexStore::add_classes(DexClasses classes) {
  m_dexen.push_back(std::move(classes));
}
