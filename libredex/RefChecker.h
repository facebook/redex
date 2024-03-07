/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "DexStore.h"
#include "FrameworkApi.h"
#include "MethodOverrideGraph.h"
#include "TypeUtil.h"

// All references occurring in some method.
struct CodeRefs {
  std::vector<const DexType*> types;
  std::vector<const DexMethod*> methods;
  std::vector<const DexField*> fields;
  bool invalid_refs{false};
  explicit CodeRefs(const DexMethod* method);
};

// Helper class that checks if it's safe to use a type/method/field in
// - the context of a particular store, and
// - any context where we can only assume a particular min-sdk.
//
// Types/methods/fields directly contained in the min-sdk are fine. We also
// check that any declaring types, array element types, super types, implemented
// interface types, return types, argument types, field types are valid for the
// given min-sdk.
//
// All functions are thread-safe.
class RefChecker {
 public:
  RefChecker() = delete;
  RefChecker(const RefChecker&) = delete;
  RefChecker& operator=(const RefChecker&) = delete;
  explicit RefChecker(const XStoreRefs* xstores,
                      size_t store_idx,
                      const api::AndroidSDK* min_sdk_api)
      : m_xstores(xstores),
        m_store_idx(store_idx),
        m_min_sdk_api(min_sdk_api) {}

  bool check_type(const DexType* type) const;

  bool check_method(const DexMethod* method) const;

  bool check_field(const DexField* field) const;

  /**
   * Check the :cls itself and its fields, methods and method code.
   * No cache for :cls because it's common to only check a definition once.
   */
  bool check_class(const DexClass* cls,
                   const std::unique_ptr<const method_override_graph::Graph>&
                       mog = nullptr) const;

  /**
   * Check :method signature and its code.
   * No cache for the :method because it's common to only check a definition
   * once.
   */
  bool check_method_and_code(const DexMethod* method) const {
    return check_method(method) && check_code_refs(CodeRefs(method));
  }

  bool check_code_refs(const CodeRefs& code_refs) const;

  bool is_in_primary_dex(const DexType* type) const;

 private:
  const XStoreRefs* m_xstores;
  size_t m_store_idx;
  const api::AndroidSDK* m_min_sdk_api;

  mutable ConcurrentMap<const DexType*, boost::optional<bool>> m_type_cache;
  mutable ConcurrentMap<const DexMethod*, boost::optional<bool>> m_method_cache;
  mutable ConcurrentMap<const DexField*, boost::optional<bool>> m_field_cache;

  bool check_type_internal(const DexType* type) const;

  bool check_method_internal(const DexMethod* method) const;

  bool check_field_internal(const DexField* field) const;
};
