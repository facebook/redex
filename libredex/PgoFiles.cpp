/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PgoFiles.h"

#include <fstream>
#include <string>
#include <vector>

#include "Debug.h"
#include "DexClass.h"

PgoFiles::PgoFiles(const folly::dynamic& config) :
    m_proguard_map(
        config.getDefault("proguard_map", "").asString().toStdString()),
    m_coldstart_class_filename(
        config.getDefault("coldstart_classes", "").asString().toStdString()),
    m_coldstart_method_filename(
        config.getDefault("coldstart_methods", "").asString().toStdString())
{
  auto no_optimizations_anno = config.find("no_optimizations_annotations");
  if (no_optimizations_anno != config.items().end()) {
    for (auto const& config_anno_name : no_optimizations_anno->second) {
      std::string anno_name = toStdString(config_anno_name.asString());
      DexType* anno = DexType::get_type(anno_name.c_str());
      if (anno) m_no_optimizations_annos.insert(anno);
    }
  }
}

/**
 * Read an interdex list file and return as a vector of appropriately-formatted
 * classname strings.
 */
std::vector<std::string> PgoFiles::load_coldstart_classes() {
  const char* kClassTail = ".class";
  const int lentail = strlen(kClassTail);
  auto file = m_coldstart_class_filename.c_str();
  
  std::vector<std::string> coldstart_classes;
  
  std::ifstream input(file);
  if (!input){
    return std::vector<std::string>();
  }
  std::string clzname;
  while (input >> clzname) {
    int position = clzname.length() - lentail;
    always_assert_log(position >= 0,
                      "Bailing, invalid class spec '%s' in interdex file %s\n",
                      clzname.c_str(), file);
    clzname.replace(position, lentail, ";");
    coldstart_classes.emplace_back(m_proguard_map.translate_class("L" + clzname));
  }
  return coldstart_classes;
}

/*
 * Read the method list file and return it is a vector of strings.
 */
std::vector<std::string> PgoFiles::load_coldstart_methods() {
  std::ifstream listfile(m_coldstart_method_filename);
  if (!listfile) {
    fprintf(stderr, "Failed to open coldstart method list: `%s'\n",
            m_coldstart_method_filename.c_str());
    return std::vector<std::string>();
  }
  std::vector<std::string> coldstart_methods;
  std::string method;
  while (std::getline(listfile, method)) {
    if (method.length() > 0) {
      coldstart_methods.push_back(m_proguard_map.translate_method(method));
    }
  }
  return coldstart_methods;
}
