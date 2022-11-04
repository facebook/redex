/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Debug.h"
#include "Pass.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"

class ScopedMemStats {
 public:
  explicit ScopedMemStats(bool enabled, bool reset) : m_enabled(enabled) {
    if (enabled) {
      if (reset) {
        try_reset_hwm_mem_stat();
      }
      auto mem_stats = get_mem_stats();
      m_before = mem_stats.vm_hwm;
      m_rss_before = mem_stats.vm_rss;
    }
  }

  void trace_log(PassManager* mgr, const Pass* pass) {
    if (m_enabled) {
      auto mem_stats = get_mem_stats();
      uint64_t after = mem_stats.vm_hwm;
      uint64_t rss_after = mem_stats.vm_rss;
      if (mgr != nullptr) {
        mgr->set_metric("vm_hwm_after", after);
        mgr->set_metric("vm_hwm_delta", after - m_before);
        mgr->set_metric("vm_rss_after", rss_after);
        mgr->set_metric("vm_rss_delta", rss_after - m_rss_before);
      }
      TRACE(STATS, 1, "VmHWM for %s was %s (%s over start).",
            pass->name().c_str(), pretty_bytes(after).c_str(),
            pretty_bytes(after - m_before).c_str());

      int64_t rss_delta =
          static_cast<int64_t>(rss_after) - static_cast<int64_t>(m_rss_before);
      const char* rss_delta_sign = "+";
      uint64_t rss_delta_abs = rss_delta;
      if (rss_delta < 0) {
        rss_delta_abs = -rss_delta;
        rss_delta_sign = "-";
      }

      TRACE(STATS, 1, "VmRSS for %s went from %s to %s (%s%s).",
            pass->name().c_str(), pretty_bytes(m_rss_before).c_str(),
            pretty_bytes(rss_after).c_str(), rss_delta_sign,
            pretty_bytes(rss_delta_abs).c_str());
    }
  }

 private:
  uint64_t m_rss_before;
  uint64_t m_before;
  bool m_enabled;
};
