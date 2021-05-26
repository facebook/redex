/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexResources.h"

class ApkResources : public AndroidResources {
 public:
  explicit ApkResources(const std::string& directory)
      : AndroidResources(directory) {}
  boost::optional<int32_t> get_min_sdk() override;
};
