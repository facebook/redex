/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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

struct ConfigFiles {
  ConfigFiles(const Json::Value& config);

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

  const std::unordered_set<DexType*> get_no_optimizations_annos() const {
    return m_no_optimizations_annos;
  }

  bool save_move_map() {
    return m_move_map;
  }

  const MethodMap& get_moved_methods_map() {
    return m_moved_methods_map;
  }

  void add_moved_methods(MethodTuple mt, DexClass* cls) {
    m_move_map = true;
    m_moved_methods_map[mt] = cls;
  }

  std::string metafile(std::string basename) {
    if (basename.empty()) return std::string();
    return outdir + '/' + basename;
  }

  ProguardMap& get_proguard_map() {
    return m_proguard_map;
  }

  std::string get_printseeds() {
    return m_printseeds;
  }

 public:
  std::string outdir;

 private:
  std::vector<std::string> load_coldstart_classes();
  std::vector<std::string> load_coldstart_methods();

 private:
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
