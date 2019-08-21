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

namespace {

bool add_if_not_present(const TypeSet& types,
                        const std::unordered_map<const DexType*, FrameworkAPI>&
                            types_to_framework_api,
                        const DexType* current_type,
                        std::unordered_set<const DexType*>* to_remove) {
  for (const DexType* type : types) {
    if (types_to_framework_api.count(type) == 0) {
      to_remove->emplace(current_type);
      return true;
    }
  }

  return false;
}

void check_and_update(
    const Scope& scope,
    std::unordered_map<const DexType*, FrameworkAPI>* types_to_framework_api) {
  TypeSystem type_system(scope);

  // Make sure that we have all the classes, up to Object in the map.
  // If not, we remove those classes. In a loop - just making sure
  // we don't miss anything.
  while (true) {
    std::unordered_set<const DexType*> to_remove;
    for (const auto& pair : *types_to_framework_api) {
      const DexType* type = pair.first;
      DexClass* cls = type_class(type);
      if (!cls) {
        to_remove.emplace(type);
        continue;
      }

      if (!is_interface(cls)) {
        TypeSet children;
        type_system.get_all_children(type, children);
        if (add_if_not_present(children, *types_to_framework_api, type,
                               &to_remove)) {
          continue;
        }

        const auto& implemented_intfs =
            type_system.get_implemented_interfaces(type);
        if (add_if_not_present(implemented_intfs, *types_to_framework_api, type,
                               &to_remove)) {
          continue;
        }

        const auto& parent_chain = type_system.parent_chain(type);
        for (const DexType* parent : parent_chain) {
          if (parent == get_object_type()) {
            continue;
          }

          if (types_to_framework_api->count(parent) == 0) {
            to_remove.emplace(type);
          }
        }
      } else {
        const auto& implementors = type_system.get_implementors(type);
        if (add_if_not_present(implementors, *types_to_framework_api, type,
                               &to_remove)) {
          continue;
        }

        TypeSet super_intfs;
        type_system.get_all_super_interfaces(type, super_intfs);
        if (add_if_not_present(super_intfs, *types_to_framework_api, type,
                               &to_remove)) {
          continue;
        }
      }
    }

    if (to_remove.empty()) {
      break;
    }

    for (const DexType* type : to_remove) {
      types_to_framework_api->erase(type);
    }
  }
}

} // namespace

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
  std::string cls_str;
  uint32_t num_extra_methods;

  while (infile >> release_library_cls >> cls_str >>
         framework_api.min_api_level >> framework_api.max_api_level >>
         num_extra_methods) {
    framework_api.cls = DexType::make_type(cls_str.c_str());
    DexType* type = DexType::get_type(release_library_cls);
    while (num_extra_methods-- > 0) {
      FrameworkMethod framework_method;
      std::string method_str;
      infile >> method_str >> framework_method.min_api_level;

      if (!type) {
        // No need to store this if the type is not used in the app.
        continue;
      }

      framework_method.mref = DexMethod::make_method(method_str);
      framework_api.methods.emplace_back(std::move(framework_method));
    }

    if (type) {
      m_types_to_framework_api[type] = std::move(framework_api);
    }
  }

  // Make sure that everything is properly setup.
  check_and_update(m_scope, &m_types_to_framework_api);
}

} // namespace api
