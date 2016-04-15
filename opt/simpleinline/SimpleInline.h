/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"
#include "DexClass.h"
#include "Resolver.h"
#include "Transform.h"

#include <unordered_map>
#include <unordered_set>

class SimpleInlinePass : public Pass {
public:
  SimpleInlinePass() : Pass("SimpleInlinePass"), virtual_inline(true) {}

  virtual void run_pass(DexClassesVector&, PgoFiles&) override;

private:
  std::unordered_set<DexMethod*> gather_non_virtual_methods(
      Scope& scope, const std::unordered_set<DexType*>& no_inline);
  void select_single_called(
      Scope& scope, std::unordered_set<DexMethod*>& methods);

private:
  // count of instructions that define a method as inlinable always
  static const size_t SMALL_CODE_SIZE = 3;

  // inline virtual methods
  bool virtual_inline;

  // set of inlinable methods
  std::unordered_set<DexMethod*> inlinable;

  // keep a map from refs to defs or nullptr if no method was found
  MethodRefCache resolved_refs;
};
