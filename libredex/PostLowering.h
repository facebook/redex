/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional/optional.hpp>
#include <string>

#include "AssetManager.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IODIMetadata.h"

class GatheredTypes;

class PostLowering {
 public:
  static std::unique_ptr<PostLowering> create();

  virtual void run(const DexStoresVector& stores) = 0;
  virtual void finalize(AssetManager& mgr) = 0;

  virtual void load_dex_indexes(ConfigFiles& conf,
                                int32_t min_sdk,
                                DexClasses* classes,
                                GatheredTypes& gtypes,
                                const std::string& store_name,
                                size_t dex_number) = 0;

  virtual ~PostLowering() = default;
};
