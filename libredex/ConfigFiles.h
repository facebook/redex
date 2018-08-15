/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>
#include <string>
#include <map>
#include <unordered_set>

#include <json/json.h>

#include "DexClass.h"
#include "ProguardMap.h"

class DexType;
using MethodTuple = std::tuple<DexString*, DexString*, DexString*>;
using MethodMap = std::map<MethodTuple, DexClass*>;

class JsonWrapper {
 public:
  explicit JsonWrapper(const Json::Value& cfg) : m_config(cfg) {}

  void get(const char* name, int64_t dflt, int64_t& param) const;

  void get(const char* name, size_t dflt, size_t& param) const;

  void get(const char* name, const std::string& dflt, std::string& param) const;

  const std::string get(const char* name, const std::string& dflt) const;

  void get(const char* name, bool dflt, bool& param) const;

  bool get(const char* name, bool dflt) const;

  void get(const char* name,
           const std::vector<std::string>& dflt,
           std::vector<std::string>& param) const;

  void get(const char* name,
           const std::vector<std::string>& dflt,
           std::unordered_set<std::string>& param) const;

  void get(
      const char* name,
      const std::unordered_map<std::string, std::vector<std::string>>& dflt,
      std::unordered_map<std::string, std::vector<std::string>>& param) const;

  void get(const char* name, const Json::Value dflt, Json::Value& param) const;

  const Json::Value get(const char* name, const Json::Value dflt) const;

  const Json::Value& operator[](const char* name) const;

 private:
  const Json::Value m_config;
};

/**
 * ConfigFiles should be a readonly structure
 */
struct ConfigFiles {
  ConfigFiles(const Json::Value& config);
  ConfigFiles(const Json::Value& config, const std::string& outdir);

  const std::vector<std::string>& get_coldstart_classes() {
    if (m_coldstart_classes.size() == 0) {
      m_coldstart_classes = load_coldstart_classes();
    }
    return m_coldstart_classes;
  }

  const std::vector<std::string>& get_coldstart_methods() {
    if (m_coldstart_methods.size() == 0) {
      m_coldstart_methods = load_coldstart_methods();
    }
    return m_coldstart_methods;
  }

  const std::unordered_set<DexType*>& get_no_optimizations_annos();

  bool save_move_map() const {
    return m_move_map;
  }

  const MethodMap& get_moved_methods_map() const {
    return m_moved_methods_map;
  }

  /* DEPRECATED! */
  void add_moved_methods(MethodTuple mt, DexClass* cls) {
    m_move_map = true;
    m_moved_methods_map[mt] = cls;
  }

  std::string metafile(const std::string& basename) const {
    if (basename.empty()) {
      return std::string();
    }
    return outdir + '/' + basename;
  }

  std::string get_outdir() const {
    return outdir;
  }

  const ProguardMap& get_proguard_map() const {
    return m_proguard_map;
  }

  const std::string& get_printseeds() const { return m_printseeds; }

  const JsonWrapper& get_json_config() const { return m_json; }

 private:
  JsonWrapper m_json;
  std::string outdir;

  std::vector<std::string> load_coldstart_classes();
  std::vector<std::string> load_coldstart_methods();

  bool m_move_map{false};
  ProguardMap m_proguard_map;
  MethodMap m_moved_methods_map;
  std::string m_coldstart_class_filename;
  std::string m_coldstart_method_filename;
  std::vector<std::string> m_coldstart_classes;
  std::vector<std::string> m_coldstart_methods;
  std::string m_printseeds; // Filename to dump computed seeds.

  // global no optimizations annotations
  std::unordered_set<DexType*> m_no_optimizations_annos;
};
