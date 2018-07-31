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
#include "InterDexPassPlugin.h"

namespace interdex {

using MethodRefs = std::unordered_set<DexMethodRef*>;
using FieldRefs = std::unordered_set<DexFieldRef*>;

enum DexStatus {
  FIRST_COLDSTART_DEX = 0,
  FIRST_EXTENDED_DEX = 1,
  SCROLL_DEX = 2,
};

struct DexConfig {
  bool is_coldstart;
  bool is_extended_set;
  bool has_scroll_cls;

  DexConfig()
      : is_coldstart(false), is_extended_set(false), has_scroll_cls(false) {}

  void reset() {
    is_coldstart = false;
    is_extended_set = false;
    has_scroll_cls = false;
  }
};

struct dex_emit_tracker {
  unsigned la_size{0};
  MethodRefs mrefs;
  FieldRefs frefs;
  std::vector<DexClass*> outs;
  std::unordered_set<DexClass*> emitted;
  std::unordered_map<std::string, DexClass*> clookup;

  void start_new_dex() {
    la_size = 0;
    mrefs.clear();
    frefs.clear();
    outs.clear();
  }
};

class InterDex {
 public:
  InterDex(const DexClassesVector& dexen,
           const std::string& mixed_mode_classes_file,
           const std::unordered_set<interdex::DexStatus, std::hash<int>>&
               mixed_mode_dex_statuses,
           ApkManager& apk_manager,
           ConfigFiles& cfg,
           std::vector<std::unique_ptr<InterDexPassPlugin>>& plugins,
           int64_t linear_alloc_limit,
           bool static_prune_classes,
           bool normal_primary_dex,
           bool can_touch_coldstart_cls,
           bool can_touch_coldstart_extended_cls,
           bool emit_scroll_set_marker,
           bool emit_canaries)
      : m_dexen(dexen),
        m_mixed_mode_classes_file(mixed_mode_classes_file),
        m_mixed_mode_dex_statuses(mixed_mode_dex_statuses),
        m_apk_manager(apk_manager),
        m_cfg(cfg),
        m_plugins(plugins),
        m_linear_alloc_limit(linear_alloc_limit),
        m_static_prune_classes(static_prune_classes),
        m_normal_primary_dex(normal_primary_dex),
        m_can_touch_coldstart_cls(can_touch_coldstart_cls),
        m_can_touch_coldstart_extended_cls(can_touch_coldstart_extended_cls),
        m_emit_scroll_set_marker(emit_scroll_set_marker),
        m_emit_canaries(emit_canaries) {}

  DexClassesVector run();

  size_t get_num_cold_start_set_dexes() const {
    return m_cold_start_set_dex_count;
  }

 private:
  void get_mixed_mode_classes();
  void emit_mixed_mode_classes();

  bool is_mixed_mode_dex(const DexConfig& dconfig);

  void flush_out_dex(dex_emit_tracker& det, DexClassesVector& outdex);

  void flush_out_secondary(dex_emit_tracker& det,
                           DexClassesVector& outdex,
                           const DexConfig& dconfig,
                           bool mixed_mode_dex = false);

  void emit_class(dex_emit_tracker& det,
                  DexClassesVector& outdex,
                  DexClass* clazz,
                  const DexConfig& dconfig,
                  bool is_primary,
                  bool check_if_skip = true);

  void emit_class(dex_emit_tracker& det,
                  DexClassesVector& outdex,
                  DexClass* clazz,
                  const DexConfig& dconfig);

  void emit_mixed_mode_classes(const std::vector<std::string>& interdexorder,
                               dex_emit_tracker& det,
                               DexClassesVector& outdex,
                               bool can_touch_interdex_order);

  const DexClassesVector& m_dexen;
  const std::string& m_mixed_mode_classes_file;
  const std::unordered_set<interdex::DexStatus, std::hash<int>>
      m_mixed_mode_dex_statuses;
  ApkManager& m_apk_manager;
  ConfigFiles& m_cfg;
  std::vector<std::unique_ptr<InterDexPassPlugin>>& m_plugins;
  int64_t m_linear_alloc_limit;
  bool m_static_prune_classes;
  bool m_normal_primary_dex;
  bool m_can_touch_coldstart_cls;
  bool m_can_touch_coldstart_extended_cls;
  bool m_emit_scroll_set_marker;
  bool m_emit_canaries;

  // Number of secondary dexes emitted.
  size_t m_secondary_dexes{0};

  // Number of coldstart dexes emitted.
  size_t m_coldstart_dexes{0};

  // Number of coldstart extended set dexes emitted.
  size_t m_extended_set_dexes{0};

  // Number of dexes containing scroll classes.
  size_t m_scroll_dexes{0};

  // Number of mixed mode dexes;
  size_t m_num_mixed_mode_dexes{0};

  size_t m_cold_start_set_dex_count{0};

  static const DexConfig s_empty_config;
};

} // namespace interdex
