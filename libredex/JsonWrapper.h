/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Json {
class Value;
} // namespace Json

class JsonWrapper {
 public:
  JsonWrapper();
  explicit JsonWrapper(const Json::Value& config);

  ~JsonWrapper();

  JsonWrapper(JsonWrapper&&) noexcept;
  JsonWrapper& operator=(JsonWrapper&&) noexcept;

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

  void get(const char* name,
           const std::unordered_map<std::string, std::string>& dflt,
           std::unordered_map<std::string, std::string>& param) const;

  void get(const char* name, const Json::Value& dflt, Json::Value& param) const;

  Json::Value get(const char* name, const Json::Value& dflt) const;

  const Json::Value& operator[](const char* name) const;

  bool contains(const char* name) const;

  const Json::Value& unwrap() const { return *m_config; }

 private:
  std::unique_ptr<Json::Value> m_config;
};
