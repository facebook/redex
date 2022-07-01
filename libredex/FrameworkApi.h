/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace api {

struct MRefInfo {
  DexMethodRef* mref;
  DexAccessFlags access_flags;

  MRefInfo(DexMethodRef* _mref, DexAccessFlags _access_flags)
      : mref(_mref), access_flags(_access_flags) {}
};

struct FRefInfo {
  DexFieldRef* fref;
  DexAccessFlags access_flags;

  FRefInfo(DexFieldRef* _fref, DexAccessFlags _access_flags)
      : fref(_fref), access_flags(_access_flags) {}
};

struct FrameworkAPI {
  DexType* cls;
  DexType* super_cls;
  std::vector<MRefInfo> mrefs_info;
  std::vector<FRefInfo> frefs_info;
  DexAccessFlags access_flags;

  bool has_method(const std::string& simple_deobfuscated_name,
                  DexProto* meth_proto,
                  DexAccessFlags meth_access_flags,
                  bool relax_access_flags_matching = false) const;

  bool has_field(const std::string& simple_deobfuscated_name,
                 DexAccessFlags field_access_flags,
                 bool relax_access_flags_matching = false) const;
};

class AndroidSDK {
 public:
  explicit AndroidSDK(boost::optional<std::string> sdk_api_file) {
    if (sdk_api_file) {
      m_sdk_api_file = *sdk_api_file;
      load_framework_classes();
    } else {
      // For missing api file, we initialize to an empty SDK.
      m_sdk_api_file = "";
    }
  }

  const std::unordered_map<const DexType*, FrameworkAPI>&
  get_framework_classes() const {
    return m_framework_classes;
  }

  bool has_method(const DexMethod* meth) const {
    auto type = meth->get_class();
    const auto& it = m_framework_classes.find(type);
    if (it == m_framework_classes.end()) {
      return false;
    }

    const auto& api = it->second;
    return api.has_method(meth->get_simple_deobfuscated_name(),
                          meth->get_proto(), meth->get_access(),
                          /* relax_access_flags_matching */ true);
  }

  bool has_field(const DexField* field) const {
    auto type = field->get_class();
    const auto& it = m_framework_classes.find(type);
    if (it == m_framework_classes.end()) {
      return false;
    }

    const auto& api = it->second;
    return api.has_field(field->get_simple_deobfuscated_name(),
                         field->get_access(),
                         /* relax_access_flags_matching */ true);
  }

  bool has_type(const DexType* type) const {
    return m_framework_classes.count(type);
  }

 private:
  void load_framework_classes();

  std::string m_sdk_api_file;
  std::unordered_map<const DexType*, FrameworkAPI> m_framework_classes;
};

} // namespace api
