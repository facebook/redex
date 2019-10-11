/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace api {

struct FrameworkAPI {
  DexType* cls;
  std::unordered_set<DexMethodRef*> mrefs;
  std::unordered_set<DexFieldRef*> frefs;
};

class ApiLevelsUtils {
 public:
  ApiLevelsUtils(const Scope& scope,
                 const std::string& framework_api_info_filename,
                 int api_level)
      : m_scope(scope),
        m_framework_api_info_filename(framework_api_info_filename),
        m_api_level(api_level) {
    load_types_to_framework_api();
  }

  const std::unordered_map<const DexType*, FrameworkAPI>&
  get_types_to_framework_api() {
    return m_types_to_framework_api;
  }

  std::unordered_map<DexType*, FrameworkAPI> get_framework_classes();

 private:
  void load_types_to_framework_api();
  void check_and_update_release_to_framework();

  const Scope& m_scope;
  std::unordered_map<const DexType*, FrameworkAPI> m_types_to_framework_api;
  std::string m_framework_api_info_filename;
  uint32_t m_api_level;
};

} // namespace api
