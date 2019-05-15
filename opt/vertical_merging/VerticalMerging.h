/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "PassManager.h"

/**
 * Merge classes vertically. (see below situation)
 * Abstract class A          class C             class E
 *     |                       |                     |
 *  class B                  class D             class F
 * If A only has one child B, then B can be merged into A
 * If C only has one child D, and C is not referenced anywhere in code,
 * then C can be merged into D.
 * If class E only has one child F, and F is not referenced anywhere in code,
 * then F can be merged into E.
 */
class VerticalMergingPass : public Pass {
 public:
  VerticalMergingPass() : Pass("VerticalMergingPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
