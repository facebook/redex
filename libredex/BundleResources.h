/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#ifdef HAS_PROTOBUF
#include "RedexResources.h"

class BundleResources : public AndroidResources {
 public:
  explicit BundleResources(const std::string& directory)
      : AndroidResources(directory) {}
  boost::optional<int32_t> get_min_sdk() override;
};
#endif // HAS_PROTOBUF
