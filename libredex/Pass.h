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

class DexClasses;
class DexClass;
class DexStore;
using DexClassesVector = std::vector<DexClasses>;
using DexStoresVector = std::vector<DexStore>;
using Scope = std::vector<DexClass*>;
class PassManager;

class PassConfig {
 public:
  explicit PassConfig(const Json::Value& cfg)
    : m_config(cfg)
  {}

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
      std::transform(str.begin(), str.end(), str.begin(), static_cast<int(*)(int)>(std::tolower));
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

  private:
    Json::Value m_config;
};

class Pass {
 public:
  enum DoesNotSync {};

  Pass(std::string name)
     : m_name(name),
       m_assumes_sync(true) {
    PassRegistry::get().register_pass(this);
  }

  Pass(std::string name, DoesNotSync)
     : m_name(name),
       m_assumes_sync(false) {
    PassRegistry::get().register_pass(this);
  }

  virtual ~Pass() {}

  bool assumes_sync() const { return m_assumes_sync; }
  std::string name() const { return m_name; }

  virtual void configure_pass(const PassConfig&) {}
  virtual void run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) = 0;

 private:
  std::string m_name;
  const bool m_assumes_sync;
};
