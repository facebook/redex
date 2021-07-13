/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include "JsonWrapper.h"

inline bool has_bundle_config(const std::string& dir) {
  std::string bundle_config =
      (boost::filesystem::path(dir) / "BundleConfig.pb").string();
  return boost::filesystem::exists(bundle_config);
}

inline bool is_apk_or_new_bundle_support_enabled(const std::string& dir,
                                                 const JsonWrapper& config) {
  if (!has_bundle_config(dir)) {
    return true;
  }
  bool result;
  config.get("enable_bleeding_edge_app_bundle_support", true, result);
  return result;
}
