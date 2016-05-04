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
#include <folly/dynamic.h>

#include "ConfigFiles.h"

class DexClasses;
class DexClass;
using DexClassesVector = std::vector<DexClasses>;
using Scope = std::vector<DexClass*>;

class PassConfig {
 public:
  explicit PassConfig(const folly::dynamic& cfg)
    : m_config(cfg)
  {}

  void get(const char* name, int64_t dflt, int64_t& param) const {
    param = m_config.getDefault(name, dflt).asInt();
  }

  void get(
    const char* name,
    const std::string& dflt,
    std::string& param
  ) const {
    param = folly::toStdString(m_config.getDefault(name, dflt).asString());
  }

  void get(const char* name, bool dflt, bool& param) const {
    param = m_config.getDefault(name, dflt).asBool();
  }

  void get(
    const char* name,
    const std::vector<std::string>& dflt,
    std::vector<std::string>& param
  ) const {
    auto it = m_config.find(name);
    if (it == m_config.items().end()) {
      param = dflt;
    } else {
      param.clear();
      for (auto const& str : it->second) {
        param.emplace_back(folly::toStdString(str.asString()));
      }
    }
  }

 private:
  folly::dynamic m_config;
};

class Pass {
 public:
  enum DoesNotSync {};

  Pass(std::string name)
     : m_name(name),
       m_assumes_sync(true) {}

  Pass(std::string name, DoesNotSync)
     : m_name(name),
       m_assumes_sync(false) {}

  virtual ~Pass() {}

  bool assumes_sync() const { return m_assumes_sync; }
  std::string name() const { return m_name; }

  virtual void configure_pass(const PassConfig&) {}
  virtual void run_pass(DexClassesVector&, ConfigFiles&) = 0;

 private:
  std::string m_name;
  const bool m_assumes_sync;
};
