/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>

#include "MetricsSink.h"
#include "PassManager.h"

// A MetricsSink that records into the metrics of the currently running pass.
class ScopedMetrics final : public MetricsSink {
 public:
  explicit ScopedMetrics(PassManager& pm) : m_pm(pm) {}

 protected:
  void report_metric(const std::string& key, int64_t value) override {
    m_pm.set_metric(key, value);
  }

 private:
  PassManager& m_pm;
};
