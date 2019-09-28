/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApiLevelsUtils.h"

#include <fstream>

#include "DexClass.h"
#include "TypeSystem.h"

namespace api {

/**
 * File format:
 *  <framework_cls> <num_methods> <num_fields>
 *      M <method0>
 *      M <method1>
 *      ...
 *      F <field0>
 *      F <field1>
 *      ...
 */
std::unordered_map<DexType*, FrameworkAPI>
ApiLevelsUtils::get_framework_classes() {
  std::ifstream infile(m_framework_api_info_filename.c_str());
  assert_log(infile, "Failed to open framework api file: %s\n",
             m_framework_api_info_filename.c_str());

  FrameworkAPI framework_api;
  std::string framework_cls_str;
  std::string class_name;
  uint32_t num_methods;
  uint32_t num_fields;

  std::unordered_map<DexType*, FrameworkAPI> framework_cls_to_api;

  while (infile >> framework_cls_str >> num_methods >> num_fields) {
    framework_api.cls = DexType::make_type(framework_cls_str.c_str());
    always_assert_log(framework_cls_to_api.count(framework_api.cls) == 0,
                      "Duplicated class name!");

    while (num_methods-- > 0) {
      std::string method_str;
      std::string tag;
      infile >> tag >> method_str;

      always_assert(tag == "M");
      DexMethodRef* mref = DexMethod::make_method(method_str);
      framework_api.mrefs.emplace(mref);
    }

    while (num_fields-- > 0) {
      std::string field_str;
      std::string tag;
      infile >> tag >> field_str;

      always_assert(tag == "F");
      DexFieldRef* fref = DexField::make_field(field_str);
      framework_api.frefs.emplace(fref);
    }

    framework_cls_to_api[framework_api.cls] = std::move(framework_api);
  }

  return framework_cls_to_api;
}

/**
 * Loads information regarding support libraries / androidX etc to framework
 * APIs.
 */
void ApiLevelsUtils::load_types_to_framework_api() {
  std::unordered_map<DexType*, FrameworkAPI> framework_cls_to_api =
      get_framework_classes();

  // TODO(emmasevastian): Actually setup release library cls to framework cls
  // map.
}

} // namespace api
