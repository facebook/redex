/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ScopedMetrics.h"

#include "PassManager.h"

namespace {

std::string cur_path(const std::vector<std::string>& segments) {
  std::string ret;
  for (const auto& s : segments) {
    if (!ret.empty()) {
      ret.append(".");
    }
    ret.append(s);
  }
  return ret;
}

} // namespace

void ScopedMetrics::set_metric(const std::string_view& key, int64_t value) {
  auto full_key = cur_path(m_segments);
  if (!full_key.empty()) {
    full_key.append(".");
  }
  full_key.append(key);
  m_pm.set_metric(full_key, value);
}
