/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"

/*
 * This class contains a set of utility functions used to build the points-to
 * semantics, mostly for stubbing external APIs. It also serves as a cache for
 * common types and methods from the standard API (like collections). Since
 * these entities are produced by the global context `g_redex`, it is better to
 * precompute them for faster retrieval. Note that we couldn't achieve this
 * using just static functions and variables, as `g_redex` is initialized at
 * runtime.
 */
class PointsToSemanticsUtils final {
 public:
  PointsToSemanticsUtils()
      : m_throwable_type(DexType::make_type("Ljava/lang/Throwable;")) {}

  PointsToSemanticsUtils(const PointsToSemanticsUtils& other) = delete;

  PointsToSemanticsUtils& operator=(const PointsToSemanticsUtils& other) =
      delete;

  DexType* get_throwable_type() const { return m_throwable_type; }

 private:
  DexType* m_throwable_type;
};
