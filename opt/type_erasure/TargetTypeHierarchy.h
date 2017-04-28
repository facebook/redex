/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "VirtualScope.h"

struct TargetTypeHierarchy {
  const char* name;
  TypeSet model_classes;
  TypeSet interfaces;

  TargetTypeHierarchy(const char* name, const Scope& scope, const DexType* root);
  TargetTypeHierarchy(const char* name, const TargetTypeHierarchy& left, const TargetTypeHierarchy& right);
  static TargetTypeHierarchy build_target_type_hierarchy(const Scope& scope);
  void print() const;
};
