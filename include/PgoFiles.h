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

#include <folly/dynamic.h>

#include "ProguardMap.h"

class DexType;

struct PgoFiles {
  PgoFiles(const folly::dynamic& config);

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

 private:
  std::vector<std::string> load_coldstart_classes();
  std::vector<std::string> load_coldstart_methods();

 private:
  ProguardMap m_proguard_map;
  std::string m_coldstart_class_filename;
  std::string m_coldstart_method_filename;
  std::vector<std::string> m_coldstart_classes;
  std::vector<std::string> m_coldstart_methods;

  // global no optimizations annotations
  std::unordered_set<DexType*> m_no_optimizations_annos;
};
