/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OutliningProfileGuidanceImpl.h"

#include "ConfigFiles.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "SourceBlocks.h"
#include "Walkers.h"

namespace outliner_impl {

using namespace outliner;

void gather_sufficiently_warm_and_hot_methods(
    const Scope& scope,
    ConfigFiles& config_files,
    PassManager& mgr,
    const ProfileGuidanceConfig& config,
    std::unordered_set<DexMethod*>* sufficiently_warm_methods,
    std::unordered_set<DexMethod*>* sufficiently_hot_methods) {
  bool has_method_profiles{false};
  if (config.use_method_profiles) {
    auto& method_profiles = config_files.get_method_profiles();
    if (method_profiles.has_stats()) {
      has_method_profiles = true;
      for (auto& p : method_profiles.all_interactions()) {
        auto& method_stats = p.second;
        walk::methods(scope,
                      [sufficiently_warm_methods,
                       sufficiently_hot_methods,
                       &method_stats,
                       &config](DexMethod* method) {
                        auto it = method_stats.find(method);
                        if (it == method_stats.end()) {
                          return;
                        }
                        if (it->second.appear_percent >=
                            config.method_profiles_appear_percent) {
                          if (it->second.call_count >
                              config.method_profiles_hot_call_count) {
                            sufficiently_hot_methods->insert(method);
                          } else if (it->second.call_count >=
                                     config.method_profiles_warm_call_count) {
                            sufficiently_warm_methods->insert(method);
                          }
                        }
                      });
      }
    }
  }

  std::unordered_set<DexType*> perf_sensitive_classes;
  if (mgr.interdex_has_run()) {
    walk::classes(scope, [&perf_sensitive_classes](DexClass* cls) {
      if (cls->is_perf_sensitive()) {
        perf_sensitive_classes.insert(cls->get_type());
      }
    });
  } else {
    for (const auto& str : config_files.get_coldstart_classes()) {
      DexType* type = DexType::get_type(str);
      if (type) {
        perf_sensitive_classes.insert(type);
      }
    }
  }

  switch (config.perf_sensitivity) {
  case PerfSensitivity::kNeverUse:
    break;

  case PerfSensitivity::kWarmWhenNoProfiles:
    if (has_method_profiles) {
      break;
    }
    FALLTHROUGH_INTENDED;
  case PerfSensitivity::kAlwaysWarm:
    walk::methods(scope,
                  [sufficiently_warm_methods,
                   &perf_sensitive_classes](DexMethod* method) {
                    if (perf_sensitive_classes.count(method->get_class())) {
                      sufficiently_warm_methods->insert(method);
                    }
                  });
    break;

  case PerfSensitivity::kHotWhenNoProfiles:
    if (has_method_profiles) {
      break;
    }
    FALLTHROUGH_INTENDED;
  case PerfSensitivity::kAlwaysHot:
    walk::methods(
        scope,
        [sufficiently_hot_methods, &perf_sensitive_classes](DexMethod* method) {
          if (perf_sensitive_classes.count(method->get_class())) {
            sufficiently_hot_methods->insert(method);
          }
        });
    break;
  }
}

PerfSensitivity parse_perf_sensitivity(const std::string& str) {
  if (str == "never") {
    return PerfSensitivity::kNeverUse;
  }
  if (str == "warm-when-no-profiles") {
    return PerfSensitivity::kWarmWhenNoProfiles;
  }
  if (str == "always-warm") {
    return PerfSensitivity::kAlwaysWarm;
  }
  if (str == "hot-when-no-profiles") {
    return PerfSensitivity::kHotWhenNoProfiles;
  }
  if (str == "always-hot") {
    return PerfSensitivity::kAlwaysHot;
  }
  always_assert_log(false, "Unknown perf sensitivity: %s", str.c_str());
}

CanOutlineBlockDecider::CanOutlineBlockDecider(
    const outliner::ProfileGuidanceConfig& config,
    bool sufficiently_warm,
    bool sufficiently_hot)
    : m_config(config),
      m_sufficiently_warm(sufficiently_warm),
      m_sufficiently_hot(sufficiently_hot) {}

CanOutlineBlockDecider::Result
CanOutlineBlockDecider::can_outline_from_big_block(
    const big_blocks::BigBlock& big_block) const {
  if (!m_sufficiently_hot && !m_sufficiently_warm) {
    return Result::CanOutline;
  }
  if (!m_sufficiently_hot) {
    always_assert(m_sufficiently_warm);
    // Make sure m_is_in_loop is initialized
    if (!m_is_in_loop) {
      m_is_in_loop.reset(
          new LazyUnorderedMap<cfg::Block*, bool>([](cfg::Block* block) {
            std::unordered_set<cfg::Block*> visited;
            std::queue<cfg::Block*> work_queue;
            for (auto e : block->succs()) {
              work_queue.push(e->target());
            }
            while (!work_queue.empty()) {
              auto other_block = work_queue.front();
              work_queue.pop();
              if (visited.insert(other_block).second) {
                if (block == other_block) {
                  return true;
                }
                for (auto e : other_block->succs()) {
                  work_queue.push(e->target());
                }
              }
            }
            return false;
          }));
    }
    if (!(*m_is_in_loop)[big_block.get_first_block()]) {
      return Result::CanOutline;
    }
  }
  // If we get here,
  // - the method is hot, or
  // - the method is not hot but warm, and the big block is in a loop
  if (m_config.block_profiles_hits < 0) {
    return m_sufficiently_hot ? Result::Hot : Result::WarmLoop;
  }
  // Make sure m_max_vals is initialized
  if (!m_max_vals) {
    m_max_vals.reset(new LazyUnorderedMap<cfg::Block*, boost::optional<float>>(
        [](cfg::Block* block) -> boost::optional<float> {
          auto* sb = source_blocks::get_first_source_block(block);
          if (sb == nullptr) {
            return boost::none;
          }
          boost::optional<float> max_val;
          sb->foreach_val([&](const auto& val_pair) {
            if (!val_pair) {
              return;
            }
            if (!max_val || (val_pair && val_pair->val > *max_val)) {
              max_val = val_pair->val;
            }
          });
          return max_val;
        }));
  }
  // Via m_max_vals, we consider the maximum hit number for each block.
  // Across all blocks, we are gathering the *minimum* of those hit numbers.
  boost::optional<float> min_val;
  for (auto block : big_block.get_blocks()) {
    auto val = (*m_max_vals)[block];
    if (!min_val || (val && *val < *min_val)) {
      min_val = val;
      if (min_val && *min_val == 0) {
        break;
      }
    }
  }
  // Let's also look back at dominators. It's beneficial if we can tighten the
  // minimum.
  auto block = big_block.get_first_block();
  auto& cfg = block->cfg();
  auto entry_block = cfg.entry_block();
  if (block != entry_block && (!min_val || *min_val != 0)) {
    if (!m_dominators) {
      m_dominators.reset(
          new dominators::SimpleFastDominators<cfg::GraphInterface>(cfg));
    }
    do {
      block = m_dominators->get_idom(block);
      auto val = (*m_max_vals)[block];
      if (!min_val || (val && *val < *min_val)) {
        min_val = val;
        if (min_val && *min_val == 0) {
          break;
        }
      }
    } while (block != entry_block);
  }
  if (!min_val) {
    return m_sufficiently_hot ? Result::HotNoSourceBlocks
                              : Result::WarmLoopNoSourceBlocks;
  }
  if (*min_val > m_config.block_profiles_hits) {
    return m_sufficiently_hot ? Result::HotExceedsThresholds
                              : Result::WarmLoopExceedsThresholds;
  }
  return Result::CanOutline;
}

} // namespace outliner_impl
