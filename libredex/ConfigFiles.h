/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "GlobalConfig.h"
#include "JsonWrapper.h"
#include "ProguardMap.h"

class DexMethodRef;
class DexType;
struct ProguardMap;

namespace Json {
class Value;
} // namespace Json

namespace api {
class AndroidSDK;
} // namespace api

namespace inliner {
struct InlinerConfig;
} // namespace inliner

namespace method_profiles {
class MethodProfiles;
} // namespace method_profiles

/**
 * ConfigFiles should be a readonly structure
 */
struct ConfigFiles {
  explicit ConfigFiles(const Json::Value& config);
  ConfigFiles(const Json::Value& config, const std::string& outdir);
  ~ConfigFiles();

  const std::vector<std::string>& get_coldstart_classes() {
    if (m_coldstart_classes.empty()) {
      m_coldstart_classes = load_coldstart_classes();
    }
    return m_coldstart_classes;
  }

  /**
   * NOTE: ONLY use if you know what you are doing!
   */
  void update_coldstart_classes(
      std::vector<std::string> new_coldstart_classes) {
    m_coldstart_classes = std::move(new_coldstart_classes);
  }

  void ensure_class_lists_loaded() {
    if (!m_load_class_lists_attempted) {
      m_load_class_lists_attempted = true;
      m_class_lists = load_class_lists();
    }
  }

  const std::unordered_map<std::string, std::vector<std::string>>&
  get_all_class_lists() {
    ensure_class_lists_loaded();
    return m_class_lists;
  }

  bool has_class_list(const std::string& name) {
    ensure_class_lists_loaded();
    return m_class_lists.count(name) != 0;
  }

  const std::vector<std::string>& get_class_list(const std::string& name) {
    ensure_class_lists_loaded();
    return m_class_lists.at(name);
  }

  const std::vector<std::string>& get_dead_class_list();

  const method_profiles::MethodProfiles& get_method_profiles() {
    ensure_agg_method_stats_loaded();
    return *m_method_profiles;
  }

  void process_unresolved_method_profile_lines();

  const std::unordered_set<DexType*>& get_no_optimizations_annos();
  const std::unordered_set<DexMethodRef*>& get_pure_methods();
  const std::unordered_set<const DexString*>& get_finalish_field_names();
  const std::unordered_set<DexType*>& get_do_not_devirt_anon();

  const std::unordered_set<std::string>&
  get_method_sorting_allowlisted_substrings() const {
    return m_method_sorting_allowlisted_substrings;
  }

  std::string metafile(const std::string& basename) const {
    if (basename.empty()) {
      return std::string();
    }
    return outdir + "/meta/" + basename;
  }

  std::string get_outdir() const { return outdir; }

  // For development only!
  void set_outdir(const std::string& new_outdir);

  const ProguardMap& get_proguard_map() const;

  const std::string& get_printseeds() const { return m_printseeds; }

  uint32_t get_instruction_size_bitwidth_limit() const {
    return m_instruction_size_bitwidth_limit;
  }

  const JsonWrapper& get_json_config() const { return m_json; }
  const GlobalConfig& get_global_config() const { return m_global_config; }

  /**
   * Get the global inliner config from the "inliner" section. If there is not
   * such section, will also look up "MethodInlinePass" section for backward
   * compatibility.
   */
  const inliner::InlinerConfig& get_inliner_config();

  boost::optional<std::string> get_android_sdk_api_file(int32_t api_level) {
    std::string api_file;
    std::string key = "android_sdk_api_" + std::to_string(api_level) + "_file";
    m_json.get(key.c_str(), "", api_file);

    if (api_file.empty()) {
      return boost::none;
    }
    return boost::optional<std::string>(api_file);
  }

  const api::AndroidSDK& get_android_sdk_api(int32_t min_sdk_api);

  void parse_global_config();

  /**
   * Load configurations with the initial scope.
   */
  void load(const Scope& scope);

  bool force_single_dex() const;

  bool emit_incoming_hashes() const;

  bool emit_outgoing_hashes() const;

  bool create_init_class_insns() const;

 private:
  JsonWrapper m_json;
  std::string outdir;
  GlobalConfig m_global_config;

  std::vector<std::string> load_coldstart_classes();
  std::unordered_map<std::string, std::vector<std::string>> load_class_lists();
  void load_method_sorting_allowlisted_substrings();
  void ensure_agg_method_stats_loaded();
  void load_inliner_config(inliner::InlinerConfig*);

  bool m_load_class_lists_attempted{false};
  std::unique_ptr<ProguardMap> m_proguard_map;
  std::string m_coldstart_class_filename;
  std::vector<std::string> m_coldstart_classes;
  std::unordered_map<std::string, std::vector<std::string>> m_class_lists;
  std::vector<std::string> m_dead_class_list;
  bool m_dead_class_list_attempted{false};
  std::unordered_set<std::string> m_method_sorting_allowlisted_substrings;
  std::string m_printseeds; // Filename to dump computed seeds.
  std::unique_ptr<method_profiles::MethodProfiles> m_method_profiles;

  // limits the output instruction size of any DexMethod to 2^n
  // 0 when limit is not present
  uint32_t m_instruction_size_bitwidth_limit;

  std::unordered_set<DexType*> m_no_devirtualize_annos;
  // global no optimizations annotations
  std::unordered_set<DexType*> m_no_optimizations_annos;
  // global pure methods
  std::unordered_set<DexMethodRef*> m_pure_methods;
  // names of fields that behave similar to final fields, i.e. written once
  // before use
  std::unordered_set<const DexString*> m_finalish_field_names;
  // Global inliner config.
  std::unique_ptr<inliner::InlinerConfig> m_inliner_config;
  // min_sdk AndroidAPI
  int32_t m_min_sdk_api_level = 0;
  std::unique_ptr<api::AndroidSDK> m_android_min_sdk_api;
};
