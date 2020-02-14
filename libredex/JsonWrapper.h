/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <json/json.h>

class JsonWrapper {
 public:
  JsonWrapper() = default;

  explicit JsonWrapper(const Json::Value& config) : m_config(config) {}

  void get(const char* name, int64_t dflt, int64_t& param) const;

  void get(const char* name, size_t dflt, size_t& param) const;

  void get(const char* name, const std::string& dflt, std::string& param) const;

  std::string get(const char* name, const std::string& dflt) const;

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

  void get(const char* name, const Json::Value& dflt, Json::Value& param) const;

  Json::Value get(const char* name, const Json::Value& dflt) const;

  const Json::Value& operator[](const char* name) const;

  bool contains(const char* name) const;

 private:
  Json::Value m_config;
};
