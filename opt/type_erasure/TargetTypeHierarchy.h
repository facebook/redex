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

  static TargetTypeHierarchy build_gql_type_hierarchy(
      const Scope& scope, const ClassHierarchy& hierarchy);
  static TargetTypeHierarchy build_cs_type_hierarchy(
      const Scope& scope, const ClassHierarchy& hierarchy);
  void print() const;
};
