/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApiLevelsUtils.h"

#include <fstream>

namespace api {

/**
 * Loads information regarding support libraries / androidX etc to framework
 * APIs.
 *
 * File format:
 *  <library_cls> <framework_cls> <min_api_level> <max_api_level>
 * <num_extra_methods> <method> <min_api_level>
 *  ....
 *
 *  Where <extra_methods> stands for methods that were added post class min_api.
 *  TODO: Revisit the format.
 */
void ApiLevelsUtils::load_types_to_framework_api() {
  std::ifstream infile(m_framework_api_info_filename.c_str());
  assert_log(infile, "Failed to open framework api file: %s\n",
             m_framework_api_info_filename.c_str());

  std::string release_library_cls;
  FrameworkAPI framework_api;
  uint32_t num_extra_methods;

  while (infile >> release_library_cls >> framework_api.cls >>
         framework_api.min_api_level >> framework_api.max_api_level >>
         num_extra_methods) {
    DexType* type = DexType::get_type(release_library_cls);
    while (num_extra_methods-- > 0) {
      FrameworkMethod framework_method;
      infile >> framework_method.method_str >> framework_method.min_api_level;

      if (!type) {
        // No need to store this if the type is not used in the app.
        continue;
      }

      framework_api.methods.emplace_back(std::move(framework_method));
    }

    if (type) {
      m_types_to_framework_api[type] = std::move(framework_api);
    }
  }
}

} // namespace api
