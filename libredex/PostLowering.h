/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional/optional.hpp>
#include <string>

#include "ApkManager.h"
#include "DexClass.h"
#include "DexStore.h"

class PostLowering {
 public:
  static std::unique_ptr<PostLowering> create();

  virtual void sync() = 0;
  virtual void run(const DexStoresVector& stores) = 0;
  virtual void finalize(ApkManager& mgr) = 0;

  virtual ~PostLowering(){};
};
