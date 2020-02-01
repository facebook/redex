/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
                  bool relax_access_flags_matching = false) const {
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
};

class AndroidSDK {
 public:
  explicit AndroidSDK(
      std::unordered_map<DexType*, FrameworkAPI> framework_classes)
      : m_framework_classes(framework_classes) {}

  bool has_method(DexMethod* meth) const {
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

 private:
  const std::unordered_map<DexType*, FrameworkAPI> m_framework_classes;
};

using TypeToFrameworkAPI = std::unordered_map<const DexType*, FrameworkAPI>;

class ApiLevelsUtils {
 public:
  ApiLevelsUtils(const Scope& scope,
                 const std::string& framework_api_info_filename,
                 int api_level)
      : m_framework_api_info_filename(framework_api_info_filename),
        m_api_level(api_level) {
    // Setting up both m_types_to_framework_api and m_framework_classes
    load_framework_api(scope);
  }

  const TypeToFrameworkAPI& get_types_to_framework_api() {
    return m_types_to_framework_api;
  }

  std::unordered_map<DexType*, FrameworkAPI> get_framework_classes();

  AndroidSDK get_android_sdk() {
    auto classes = get_framework_classes();
    AndroidSDK sdk(std::move(classes));
    return sdk;
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

  /**
   * NOTE: Those work as "non-private" in the sense that we check where
   *       they are referenced:
   */
  std::unordered_set<DexMethodRef*> m_methods_non_private;
  std::unordered_set<DexFieldRef*> m_fields_non_private;
};

} // namespace api
