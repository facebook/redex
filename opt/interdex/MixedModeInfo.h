/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "AssetManager.h"
#include "DexClass.h"

namespace interdex {

enum DexStatus {
  FIRST_COLDSTART_DEX = 0,
  FIRST_EXTENDED_DEX = 1,
  SCROLL_DEX = 2,
};

enum MixedModeType {
  PRE_DEFINED_DEXES = 0,
};

class MixedModeInfo {
 public:
  bool has_status(DexStatus status) const {
    return m_type == MixedModeType::PRE_DEFINED_DEXES &&
           m_mixed_mode_dex_statuses.count(status) > 0;
  }

  void set_mixed_mode_dex_statuses(
      std::unordered_set<interdex::DexStatus, std::hash<int>>&&
          mixed_mode_dex_statuses) {
    m_type = MixedModeType::PRE_DEFINED_DEXES;
    m_mixed_mode_dex_statuses = std::move(mixed_mode_dex_statuses);
  }

 private:
  MixedModeType m_type;
  std::unordered_set<DexStatus, std::hash<int>> m_mixed_mode_dex_statuses;
};

} // namespace interdex
