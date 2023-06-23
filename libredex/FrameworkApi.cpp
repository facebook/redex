/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FrameworkApi.h"

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <istream>
#include <sstream>

namespace api {

bool FrameworkAPI::has_method(const std::string& simple_deobfuscated_name,
                              DexProto* meth_proto,
                              DexAccessFlags meth_access_flags,
                              bool relax_access_flags_matching) const {
  for (const MRefInfo& mref_info : mrefs_info) {
    auto* mref = mref_info.mref;
    if (mref->get_proto() != meth_proto ||
        mref->get_name()->str() != simple_deobfuscated_name) {
      continue;
    }

    // We also need to check the access flags.
    // NOTE: We accept cases where the methods are not declared final.
    if (meth_access_flags == mref_info.access_flags ||
        (meth_access_flags & ~ACC_FINAL) == mref_info.access_flags) {
      return true;
    }
    // There are mismatches on the higher bits of the access flags on some
    // methods between the API file generated using dex.py and what we have in
    // Redex, even if they are the 'same' method.
    // In the method presence check, we relax the matching to only
    // the last 4 bits that includes PUBLIC, PRIVATE, PROTECTED and STATIC.
    if (relax_access_flags_matching) {
      auto masked_info_access = 0xF & mref_info.access_flags;
      auto masked_meth_access = 0xF & meth_access_flags;
      if (masked_info_access == masked_meth_access) {
        return true;
      }
    }
  }
  return false;
}

bool FrameworkAPI::has_field(const std::string& simple_deobfuscated_name,
                             DexAccessFlags field_access_flags,
                             bool relax_access_flags_matching) const {
  for (const FRefInfo& fref_info : frefs_info) {
    auto* fref = fref_info.fref;
    if (fref->get_name()->str() != simple_deobfuscated_name) {
      continue;
    }

    // We also need to check the access flags.
    // NOTE: We accept cases where the fields are not declared final.
    if (field_access_flags == fref_info.access_flags ||
        (field_access_flags & ~ACC_FINAL) == fref_info.access_flags) {
      return true;
    }
    // There are mismatches on the higher bits of the access flags on some
    // fields between the API file generated using dex.py and what we have in
    // Redex, even if they are the 'same' field.
    // In the field presence check, we relax the matching to only
    // the last 4 bits that includes PUBLIC, PRIVATE, PROTECTED and STATIC.
    if (relax_access_flags_matching) {
      auto masked_info_access = 0xF & fref_info.access_flags;
      auto masked_field_access = 0xF & field_access_flags;
      if (masked_info_access == masked_field_access) {
        return true;
      }
    }
  }
  return false;
}

/**
 * File format:
 *  <framework_cls> <access_flags> <super_cls> <num_methods> <num_fields>
 *      M <method0>
 *      M <method1>
 *      ...
 *      F <field0>
 *      F <field1>
 *      ...
 */

namespace {

void parse_framework_description(
    std::istream& input,
    std::unordered_map<const DexType*, FrameworkAPI>* framework_classes) {
  std::string framework_cls_str;
  std::string super_cls_str;
  std::string class_name;
  uint32_t num_methods;
  uint32_t num_fields;
  uint32_t access_flags;

  while (input >> framework_cls_str >> access_flags >> super_cls_str >>
         num_methods >> num_fields) {
    FrameworkAPI framework_api;
    framework_api.cls = DexType::make_type(framework_cls_str);
    always_assert_log(framework_classes->count(framework_api.cls) == 0,
                      "Duplicated class name!");
    framework_api.super_cls = DexType::make_type(super_cls_str);
    framework_api.access_flags = DexAccessFlags(access_flags);

    while (num_methods-- > 0) {
      std::string method_str;
      std::string tag;
      uint32_t m_access_flags;

      input >> tag >> method_str >> m_access_flags;

      always_assert(tag == "M");
      DexMethodRef* mref = DexMethod::make_method(method_str);
      framework_api.mrefs_info.emplace_back(mref,
                                            DexAccessFlags(m_access_flags));
    }

    while (num_fields-- > 0) {
      std::string field_str;
      std::string tag;
      uint32_t f_access_flags;

      input >> tag >> field_str >> f_access_flags;

      always_assert(tag == "F");
      DexFieldRef* fref = DexField::make_field(field_str);
      framework_api.frefs_info.emplace_back(fref,
                                            DexAccessFlags(f_access_flags));
    }

    auto& map_entry = (*framework_classes)[framework_api.cls];
    map_entry = std::move(framework_api);
  }
  always_assert_log(!framework_classes->empty(),
                    "Failed to load any class from the framework api file");
}

} // namespace

AndroidSDK AndroidSDK::from_string(const std::string& input) {
  AndroidSDK sdk{};

  std::istringstream iss{input};
  parse_framework_description(iss, &sdk.m_framework_classes);

  return sdk;
}

void AndroidSDK::load_framework_classes() {
  std::ifstream infile(m_sdk_api_file.c_str());
  assert_log(infile, "Failed to open framework api file: %s\n",
             m_sdk_api_file.c_str());

  parse_framework_description(infile, &m_framework_classes);
}

} // namespace api
