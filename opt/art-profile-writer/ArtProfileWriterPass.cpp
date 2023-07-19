/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* This pass optionally creates a baseline profile file in a superset of the
 * human-readable ART profile format (HRF) according to
 * https://developer.android.com/topic/performance/baselineprofiles/manually-create-measure#define-rules-manually
 * .
 */

#include "ArtProfileWriterPass.h"

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <string>

#include "ConfigFiles.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"

namespace {
const std::string BASELINE_PROFILES_FILE = "additional-baseline-profiles.list";

struct ArtProfileEntryFlags {
  bool hot{false};
  bool startup{false};
  bool not_startup{false};
};
} // namespace

std::ostream& operator<<(std::ostream& os, const ArtProfileEntryFlags& flags) {
  if (flags.hot) {
    os << "H";
  }
  if (flags.startup) {
    os << "S";
  }
  if (flags.not_startup) {
    os << "P";
  }
  return os;
}

void ArtProfileWriterPass::bind_config() {
  bind("perf_appear100_threshold", m_perf_config.appear100_threshold,
       m_perf_config.appear100_threshold);
  bind("perf_call_count_threshold", m_perf_config.call_count_threshold,
       m_perf_config.call_count_threshold);
  bind("perf_coldstart_appear100_threshold", m_perf_config.appear100_threshold,
       m_perf_config.coldstart_appear100_threshold);
  bind("perf_interactions", m_perf_config.interactions,
       m_perf_config.interactions);
}

void ArtProfileWriterPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  const auto& method_profiles = conf.get_method_profiles();
  std::unordered_map<const DexMethodRef*, ArtProfileEntryFlags> method_flags;
  for (auto& interaction_id : m_perf_config.interactions) {
    bool startup = interaction_id == "ColdStart";
    const auto& method_stats = method_profiles.method_stats(interaction_id);
    for (auto&& [method, stat] : method_stats) {
      if (stat.appear_percent >=
              (startup ? m_perf_config.coldstart_appear100_threshold
                       : m_perf_config.appear100_threshold) &&
          stat.call_count >= m_perf_config.call_count_threshold) {
        auto& mf = method_flags[method];
        mf.hot = true;
        if (startup) {
          // consistent with buck python config in the post-process baseline
          // profile generator, which is set both flags true for ColdStart
          // methods
          mf.startup = true;
          mf.not_startup = true;
        } else {
          mf.not_startup = true;
        }
      }
    }
  }

  std::ofstream ofs{conf.metafile(BASELINE_PROFILES_FILE)};

  always_assert(!stores.empty());
  auto& dexen = stores.front().get_dexen();
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  mgr.incr_metric("min_sdk", min_sdk);
  auto end = min_sdk >= 21 ? dexen.size() : 1;
  size_t methods_with_baseline_profile = 0;
  for (size_t dex_idx = 0; dex_idx < end; dex_idx++) {
    auto& dex = dexen.at(dex_idx);
    for (auto* cls : dex) {
      for (auto* method : cls->get_all_methods()) {
        auto it = method_flags.find(method);
        if (it == method_flags.end()) {
          continue;
        }
        std::string descriptor = show_deobfuscated(method);
        // reformat it into manual profile pattern so baseline profile generator
        // in post-process can recognize the method
        boost::replace_all(descriptor, ".", "->");
        boost::replace_all(descriptor, ":(", "(");
        ofs << it->second << descriptor << std::endl;
        methods_with_baseline_profile++;
      }
    }
  }

  mgr.incr_metric("methods_with_baseline_profile",
                  methods_with_baseline_profile);
}

static ArtProfileWriterPass s_pass;
