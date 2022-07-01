/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JsonWrapper.h"

#include <algorithm>
#include <json/value.h>
#include <stdexcept>

JsonWrapper::JsonWrapper() : JsonWrapper(Json::nullValue) {}
JsonWrapper::JsonWrapper(const Json::Value& config)
    : m_config(new Json::Value(config)) {}

JsonWrapper::~JsonWrapper() {}

JsonWrapper::JsonWrapper(JsonWrapper&& other) noexcept
    : m_config(std::move(other.m_config)) {}
JsonWrapper& JsonWrapper::operator=(JsonWrapper&& rhs) noexcept {
  m_config = std::move(rhs.m_config);
  return *this;
}

void JsonWrapper::get(const char* name, int64_t dflt, int64_t& param) const {
  param = m_config->get(name, (Json::Int64)dflt).asInt();
}

void JsonWrapper::get(const char* name, size_t dflt, size_t& param) const {
  param = m_config->get(name, (Json::UInt)dflt).asUInt();
}

void JsonWrapper::get(const char* name,
                      const std::string& dflt,
                      std::string& param) const {
  param = m_config->get(name, dflt).asString();
}

std::string JsonWrapper::get(const char* name, const std::string& dflt) const {
  return m_config->get(name, dflt).asString();
}

void JsonWrapper::get(const char* name, bool dflt, bool& param) const {
  auto val = m_config->get(name, dflt);

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
  auto it = (*m_config)[name];
  // NOLINTNEXTLINE(readability-container-size-empty)
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
  auto it = (*m_config)[name];
  param.clear();
  // NOLINTNEXTLINE(readability-container-size-empty)
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
  auto cfg = (*m_config)[name];
  param.clear();
  // NOLINTNEXTLINE(readability-container-size-empty)
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

void JsonWrapper::get(
    const char* name,
    const std::unordered_map<std::string, std::string>& dflt,
    std::unordered_map<std::string, std::string>& param) const {
  auto cfg = (*m_config)[name];
  param.clear();
  // NOLINTNEXTLINE(readability-container-size-empty)
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
      auto val = *it;
      if (!val.isString()) {
        throw std::runtime_error("Cannot convert JSON value to string: " +
                                 val.asString());
      }

      param[key.asString()] = val.asString();
    }
  }
}

void JsonWrapper::get(const char* name,
                      const Json::Value& dflt,
                      Json::Value& param) const {
  param = m_config->get(name, dflt);
}

Json::Value JsonWrapper::get(const char* name, const Json::Value& dflt) const {
  return m_config->get(name, dflt);
}

const Json::Value& JsonWrapper::operator[](const char* name) const {
  return (*m_config)[name];
}

bool JsonWrapper::contains(const char* name) const {
  return m_config->isMember(name);
}
