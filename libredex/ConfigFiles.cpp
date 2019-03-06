/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConfigFiles.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "Debug.h"
#include "DexClass.h"

void JsonWrapper::get(const char* name, int64_t dflt, int64_t& param) const {
  param = m_config.get(name, (Json::Int64)dflt).asInt();
}

void JsonWrapper::get(const char* name, size_t dflt, size_t& param) const {
  param = m_config.get(name, (Json::UInt)dflt).asUInt();
}

void JsonWrapper::get(const char* name,
                     const std::string& dflt,
                     std::string& param) const {
  param = m_config.get(name, dflt).asString();
}

const std::string JsonWrapper::get(const char* name,
                                   const std::string& dflt) const {
  return m_config.get(name, dflt).asString();
}

void JsonWrapper::get(const char* name, bool dflt, bool& param) const {
  auto val = m_config.get(name, dflt);

  // Do some simple type conversions that folly used to do
  if (val.isBool()) {
    param = val.asBool();
    return;
  } else if (val.isInt()) {
    auto valInt = val.asInt();
    if (valInt == 0 || valInt == 1) {
      param = (val.asInt() != 0);
      return;
    }
  } else if (val.isString()) {
    auto str = val.asString();
    std::transform(str.begin(), str.end(), str.begin(),
                   [](auto c) { return ::tolower(c); });
    if (str == "0" || str == "false" || str == "off" || str == "no") {
      param = false;
      return;
    } else if (str == "1" || str == "true" || str == "on" || str == "yes") {
      param = true;
      return;
    }
  }
  throw std::runtime_error("Cannot convert JSON value to bool: " +
                           val.asString());
}

bool JsonWrapper::get(const char* name, bool dflt) const {
  bool res;
  get(name, dflt, res);
  return res;
}

void JsonWrapper::get(const char* name,
                     const std::vector<std::string>& dflt,
                     std::vector<std::string>& param) const {
  auto it = m_config[name];
  if (it == Json::nullValue) {
    param = dflt;
  } else {
    param.clear();
    for (auto const& str : it) {
      param.emplace_back(str.asString());
    }
  }
}

void JsonWrapper::get(const char* name,
                     const std::vector<std::string>& dflt,
                     std::unordered_set<std::string>& param) const {
  auto it = m_config[name];
  param.clear();
  if (it == Json::nullValue) {
    param.insert(dflt.begin(), dflt.end());
  } else {
    for (auto const& str : it) {
      param.emplace(str.asString());
    }
  }
}

void JsonWrapper::get(
    const char* name,
    const std::unordered_map<std::string, std::vector<std::string>>& dflt,
    std::unordered_map<std::string, std::vector<std::string>>& param) const {
  auto cfg = m_config[name];
  param.clear();
  if (cfg == Json::nullValue) {
    param = dflt;
  } else {
    if (!cfg.isObject()) {
      throw std::runtime_error("Cannot convert JSON value to object: " +
                               cfg.asString());
    }
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
      auto key = it.key();
      if (!key.isString()) {
        throw std::runtime_error("Cannot convert JSON value to string: " +
                                 key.asString());
      }
      auto& val = *it;
      if (!val.isArray()) {
        throw std::runtime_error("Cannot convert JSON value to array: " +
                                 val.asString());
      }
      for (auto& str : val) {
        if (!str.isString()) {
          throw std::runtime_error("Cannot convert JSON value to string: " +
                                   str.asString());
        }
        param[key.asString()].push_back(str.asString());
      }
    }
  }
}

void JsonWrapper::get(const char* name,
                     const Json::Value dflt,
                     Json::Value& param) const {
  param = m_config.get(name, dflt);
}

const Json::Value JsonWrapper::get(const char* name,
                                   const Json::Value dflt) const {
  return m_config.get(name, dflt);
}

const Json::Value& JsonWrapper::operator[](const char* name) const {
  return m_config[name];
}

ConfigFiles::ConfigFiles(const Json::Value& config, const std::string& outdir)
    : m_json(config),
      outdir(outdir),
      m_proguard_map(config.get("proguard_map", "").asString()),
      m_coldstart_class_filename(
          config.get("coldstart_classes", "").asString()),
      m_coldstart_method_filename(
          config.get("coldstart_methods", "").asString()),
      m_profiled_methods_filename(
          config.get("profiled_methods_file", "").asString()),
      m_printseeds(config.get("printseeds", "").asString()) {

  if (m_profiled_methods_filename != "") {
    load_method_to_weight();
  }
  load_method_sorting_whitelisted_substrings();
  uint32_t instruction_size_bitwidth_limit =
      config.get("instruction_size_bitwidth_limit", 0).asUInt();
  always_assert_log(
      instruction_size_bitwidth_limit < 32,
      "instruction_size_bitwidth_limit must be between 0 and 31, actual: %u\n",
      instruction_size_bitwidth_limit);
  m_instruction_size_bitwidth_limit = instruction_size_bitwidth_limit;
}

ConfigFiles::ConfigFiles(const Json::Value& config) : ConfigFiles(config, "") {}

/**
 * This function relies on the g_redex.
 */
const std::unordered_set<DexType*>& ConfigFiles::get_no_optimizations_annos() {
  if (m_no_optimizations_annos.empty()) {
    Json::Value no_optimizations_anno;
    m_json.get("no_optimizations_annotations", Json::nullValue,
               no_optimizations_anno);
    if (no_optimizations_anno != Json::nullValue) {
      for (auto const& config_anno_name : no_optimizations_anno) {
        std::string anno_name = config_anno_name.asString();
        DexType* anno = DexType::get_type(anno_name.c_str());
        if (anno) m_no_optimizations_annos.insert(anno);
      }
    }
  }
  return m_no_optimizations_annos;
}

/**
 * Read an interdex list file and return as a vector of appropriately-formatted
 * classname strings.
 */
std::vector<std::string> ConfigFiles::load_coldstart_classes() {
  const char* kClassTail = ".class";
  const size_t lentail = strlen(kClassTail);
  auto file = m_coldstart_class_filename.c_str();

  std::vector<std::string> coldstart_classes;

  std::ifstream input(file);
  if (!input){
    return std::vector<std::string>();
  }
  std::string clzname;
  while (input >> clzname) {
		long position = clzname.length() - lentail;
    always_assert_log(position >= 0,
                      "Bailing, invalid class spec '%s' in interdex file %s\n",
                      clzname.c_str(), file);
    clzname.replace(position, lentail, ";");
    coldstart_classes.emplace_back(m_proguard_map.translate_class("L" + clzname));
  }
  return coldstart_classes;
}

/**
 * Read a map of {list_name : class_list} from json
 */
std::unordered_map<std::string, std::vector<std::string> > ConfigFiles::load_class_lists() {
  std::unordered_map<std::string, std::vector<std::string> > lists;
  std::string class_lists_filename;
  this->m_json.get("class_lists", "", class_lists_filename);

  if (class_lists_filename.empty()) {
    return lists;
  }

  std::ifstream input(class_lists_filename);
  Json::Reader reader;
  Json::Value root;
  bool parsing_succeeded = reader.parse(input, root);
  always_assert_log(parsing_succeeded, "Failed to parse class list json from file: %s\n%s",
                    class_lists_filename.c_str(),
                    reader.getFormattedErrorMessages().c_str());

  for (Json::ValueIterator it = root.begin(); it != root.end(); ++it) {
    std::vector<std::string> class_list;
    Json::Value current_list = *it;
    for (Json::ValueIterator list_it = current_list.begin(); list_it != current_list.end(); ++list_it) {
      lists[it.key().asString()].push_back((*list_it).asString());
    }
  }

  lists["secondary_dex_head.list"] = get_coldstart_classes();

  return lists;
}

/*
 * Read the method list file and return it is a vector of strings.
 */
std::vector<std::string> ConfigFiles::load_coldstart_methods() {
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

void ConfigFiles::load_method_to_weight() {
  std::ifstream infile(m_profiled_methods_filename.c_str());
  assert_log(infile, "Can't open method profile file: %s\n",
             m_profiled_methods_filename.c_str());

  std::string deobfuscated_name;
  unsigned int weight;
  TRACE(CUSTOMSORT, 2, "Setting sort start file %s\n",
        m_profiled_methods_filename.c_str());

  unsigned int count = 0;
  while (infile >> deobfuscated_name >> weight) {
    m_method_to_weight[deobfuscated_name] = weight;
    count++;
  }

  assert_log(count > 0, "Method profile file %s didn't contain valid entries\n",
             m_profiled_methods_filename.c_str());
  TRACE(CUSTOMSORT, 2, "Preset sort weight count=%d\n", count);
}

void ConfigFiles::load_method_sorting_whitelisted_substrings() {
  const auto json_cfg = get_json_config();
  Json::Value json_result;
  json_cfg.get("method_sorting_whitelisted_substrings", Json::nullValue,
               json_result);
  if (json_result != Json::nullValue) {
    for (auto const& json_element : json_result) {
      m_method_sorting_whitelisted_substrings.insert(json_element.asString());
    }
  }
}
