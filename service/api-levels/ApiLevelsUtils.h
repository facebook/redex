/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "FrameworkApi.h"

namespace api {

using TypeToFrameworkAPI = std::unordered_map<const DexType*, FrameworkAPI>;

class ApiLevelsUtils {
 public:
  ApiLevelsUtils(const Scope& scope,
                 const std::string& framework_api_info_filename,
                 uint32_t api_level)
      : m_framework_api_info_filename(framework_api_info_filename),
        m_api_level(api_level),
        m_sdk_api(framework_api_info_filename) {
    // Setting up both m_types_to_framework_api and m_framework_classes
    load_framework_api(scope);
  }

  const TypeToFrameworkAPI& get_types_to_framework_api() {
    return m_types_to_framework_api;
  }

  std::unordered_map<DexType*, FrameworkAPI> get_framework_classes() {
    return m_sdk_api.get_framework_classes();
  }

  /**
   * NOTE: Workaround for the fact that real private members can be made public
   * by any pass ... We gather:
   *  - members that are accessed outside of their own class
   *  - true virtual methods
   *
   * NOTE: This needs to run every time something changes in the scope.
   */
  void gather_non_private_members(const Scope& scope);

  void filter_types(const std::unordered_set<const DexType*>& types,
                    const Scope& scope);

 private:
  void load_framework_api(const Scope& scope);
  void check_and_update_release_to_framework(const Scope& scope);

  TypeToFrameworkAPI m_types_to_framework_api;
  std::unordered_set<DexType*> m_framework_classes;
  std::string m_framework_api_info_filename;
  uint32_t m_api_level;
  api::AndroidSDK m_sdk_api;

  /**
   * NOTE: Those work as "non-private" in the sense that we check where
   *       they are referenced:
   */
  std::unordered_set<DexMethodRef*> m_methods_non_private;
  std::unordered_set<DexFieldRef*> m_fields_non_private;
};

} // namespace api
