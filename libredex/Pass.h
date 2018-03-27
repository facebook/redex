/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <string>
#include <vector>
#include <json/json.h>
#include <iostream>
#include <algorithm>

#include "DexStore.h"
#include "ConfigFiles.h"
#include "PassRegistry.h"

class PassManager;

class PassConfig {
 public:
  explicit PassConfig(const Json::Value& cfg) : m_config(cfg) {}

  void get(const char* name, int64_t dflt, int64_t& param) const {
    param = m_config.get(name, (Json::Int64)dflt).asInt();
  }

  void get(
    const char* name,
    const std::string& dflt,
    std::string& param
  ) const {
    param = m_config.get(name, dflt).asString();
  }

  void get(const char* name, bool dflt, bool& param) const {
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
      std::transform(str.begin(), str.end(), str.begin(), [](auto c) {
        return ::tolower(c);
      });
      if (str == "0" || str == "false" || str == "off" || str == "no") {
        param = false;
        return;
      } else if (str == "1" || str == "true" || str == "on" || str == "yes") {
        param = true;
        return;
      }
    }
    throw std::runtime_error("Cannot convert JSON value to bool: " + val.asString());
  }

  void get(
    const char* name,
    const std::vector<std::string>& dflt,
    std::vector<std::string>& param
  ) const {
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

  void get(
    const char* name,
    const std::vector<std::string>& dflt,
    std::unordered_set<std::string>& param
  ) const {
    auto it = m_config[name];
    param.clear();
    if (it == Json::nullValue) {
      param.insert(
          dflt.begin(),
          dflt.end());
    } else {
      for (auto const& str : it) {
        param.emplace(str.asString());
      }
    }
  }

  void get(
           const char* name,
           const std::unordered_map<std::string, std::vector<std::string>>& dflt,
           std::unordered_map<std::string, std::vector<std::string>>& param
           ) const {
    auto cfg = m_config[name];
    param.clear();
    if (cfg == Json::nullValue) {
      param = dflt;
    } else {
      if (!cfg.isObject()) {
        throw std::runtime_error("Cannot convert JSON value to object: " + cfg.asString());
      }
      for (auto it = cfg.begin() ; it != cfg.end() ; ++it) {
        auto key = it.key();
        if (!key.isString()) {
          throw std::runtime_error("Cannot convert JSON value to string: " + key.asString());
        }
        auto& val = *it;
        if (!val.isArray()) {
          throw std::runtime_error("Cannot convert JSON value to array: " + val.asString());
        }
        for (auto& str : val) {
          if (!str.isString()) {
            throw std::runtime_error("Cannot convert JSON value to string: " + str.asString());
          }
          param[key.asString()].push_back(str.asString());
        }
      }
    }
  }

  void get(const char* name, const Json::Value dflt, Json::Value& param) const {
    param = m_config.get(name, dflt);
  }

 private:
  Json::Value m_config;
};

class Pass {
 public:

  Pass(const std::string& name)
     : m_name(name) {
    PassRegistry::get().register_pass(this);
  }

  virtual ~Pass() {}

  std::string name() const { return m_name; }

  virtual void configure_pass(const PassConfig&) {}

  /**
   * All passes' eval_pass are run, and then all passes' run_pass are run. This allows each
   * pass to evaluate its rules in terms of the original input, without other passes changing
   * the identity of classes. You should NOT change anything in the dex stores in eval_pass.
   * There is no protection against doing so, this is merely a convention.
   */

  virtual void eval_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {};
  virtual void run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) = 0;

 private:
  std::string m_name;
};
