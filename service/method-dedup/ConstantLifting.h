/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"

class TypeTags;

class ConstantLifting {

 public:
  ConstantLifting();

  bool is_applicable_to_constant_lifting(const DexMethod* method);

  // The potentially created constant value stub methods are returned.
  std::vector<DexMethod*> lift_constants_from(
      const Scope& scope,
      const TypeTags* type_tags,
      const std::vector<DexMethod*>& methods,
      const size_t stud_method_threshold);

  uint32_t get_num_const_lifted_methods() const {
    return m_num_const_lifted_methods;
  }

 private:
  const DexType* s_method_meta_anno;
  uint32_t m_num_const_lifted_methods;
};
