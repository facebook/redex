/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "ApkManager.h"
#include "DexClass.h"

namespace interdex {

enum DexStatus {
  FIRST_COLDSTART_DEX = 0,
  FIRST_EXTENDED_DEX = 1,
  SCROLL_DEX = 2,
};

enum MixedModeType {
  PRE_DEFINED_DEXES = 0,
  PRE_DEFINED_CLASSES = 1,
};

class MixedModeInfo {
 public:
  bool has_predefined_classes() const {
    return m_type == MixedModeType::PRE_DEFINED_CLASSES;
  }

  bool is_mixed_mode_class(DexClass* clazz) const {
    return m_type == MixedModeType::PRE_DEFINED_CLASSES &&
           m_mixed_mode_classes.count(clazz);
  }

  bool has_status(DexStatus status) const {
    return m_type == MixedModeType::PRE_DEFINED_DEXES &&
           m_mixed_mode_dex_statuses.count(status) > 0;
  }

  const std::unordered_set<DexClass*>& get_mixed_mode_classes() const {
    always_assert(m_type == MixedModeType::PRE_DEFINED_CLASSES);
    return m_mixed_mode_classes;
  }

  bool can_touch_coldstart_set() const {
    return m_type == MixedModeType::PRE_DEFINED_CLASSES &&
           m_can_touch_coldstart_set;
  }

  bool can_touch_coldstart_extended_set() const {
    return m_type == MixedModeType::PRE_DEFINED_CLASSES &&
           m_can_touch_coldstart_extended_set;
  }

  void set_mixed_mode_dex_statuses(
      std::unordered_set<interdex::DexStatus, std::hash<int>>&&
          mixed_mode_dex_statuses) {
    m_type = MixedModeType::PRE_DEFINED_DEXES;
    m_mixed_mode_dex_statuses = std::move(mixed_mode_dex_statuses);
  }

  void set_mixed_mode_classes(std::unordered_set<DexClass*>&& mixed_mode_classes,
                              bool can_touch_coldstart_set,
                              bool can_touch_coldstart_extended_set) {
    m_type = MixedModeType::PRE_DEFINED_CLASSES;

    m_mixed_mode_classes = std::move(mixed_mode_classes);
    m_can_touch_coldstart_set = can_touch_coldstart_set;
    m_can_touch_coldstart_extended_set = can_touch_coldstart_extended_set;
  }

  void remove_mixed_mode_class(DexClass* clazz) {
    always_assert(m_type == MixedModeType::PRE_DEFINED_CLASSES);
    m_mixed_mode_classes.erase(clazz);
  }

  void remove_all_mixed_mode_classes() {
    always_assert(m_type == MixedModeType::PRE_DEFINED_CLASSES);
    m_mixed_mode_classes.clear();
  }

 private:
  MixedModeType m_type;
  std::unordered_set<DexClass*> m_mixed_mode_classes;
  std::unordered_set<DexStatus, std::hash<int>> m_mixed_mode_dex_statuses;
  bool m_can_touch_coldstart_set;
  bool m_can_touch_coldstart_extended_set;
};

} // namespace interdex
