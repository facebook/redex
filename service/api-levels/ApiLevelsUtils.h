/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace api {

struct FrameworkMethod {
  std::string method_str;
  uint32_t min_api_level;
};

struct FrameworkAPI {
  std::string cls;
  uint32_t min_api_level;
  uint32_t max_api_level;
  // Contains
  std::vector<FrameworkMethod> methods;
};

class ApiLevelsUtils {
 public:
  ApiLevelsUtils(const std::string& framework_api_info_filename)
      : m_framework_api_info_filename(framework_api_info_filename) {}

  const std::unordered_map<DexType*, FrameworkAPI>&
  get_types_to_framework_api() {
    if (m_types_to_framework_api.size() == 0) {
      load_types_to_framework_api();
    }

    return m_types_to_framework_api;
  }

 private:
  void load_types_to_framework_api();

  std::unordered_map<DexType*, FrameworkAPI> m_types_to_framework_api;
  std::string m_framework_api_info_filename;
};

} // namespace api
