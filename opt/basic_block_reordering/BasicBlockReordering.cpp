/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BasicBlockReordering.h"

#include <atomic>

#include "ControlFlow.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "SourceBlocks.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_NUM_BLOCKS_DEFERRED = "num_blocks_deferred";
constexpr const char* METRIC_NUM_TOTAL_BLOCKS = "num_total_blocks";
constexpr const char* METRIC_NUM_SKIPPED_METHODS = "num_skipped_methods";

struct ProfileGuidedLinearization : public cfg::LinearizationStrategy {
 public:
  explicit ProfileGuidedLinearization(
      const BasicBlockReorderingPass::Config& config)
      : m_config(config) {}

  std::vector<cfg::Block*> order(
      cfg::ControlFlowGraph& cfg,
      sparta::WeakTopologicalOrdering<cfg::BlockChain*> wto) override {

    std::vector<cfg::Block*> main_order;
    std::vector<cfg::Block*> deferred_blocks;
    main_order.reserve(cfg.blocks().size());

    auto chain_has_low_appearance = [this, &cfg](cfg::BlockChain* c) {
      for (cfg::Block* b : *c) {
        if (b == cfg.entry_block()) {
          return false;
        }
        auto source_blocks = source_blocks::gather_source_blocks(b);
        // Find the first source block in the chain.
        if (!source_blocks.empty()) {
          auto representative = *(source_blocks.begin());
          boost::optional<float> val =
              representative->get_val(m_config.interaction_profile);
          if (val) {
            return *val <= m_config.low_appearance_threshold;
          } else {
            return true;
          }
        }
      }
      return true;
    };

    auto profile_order = [&](cfg::BlockChain* c) {
      if (c->empty()) {
        return;
      }
      // Defer the chain if first block in the chain has low appear rate.
      if (chain_has_low_appearance(c)) {
        deferred_blocks.insert(deferred_blocks.end(), c->begin(), c->end());
      } else {
        main_order.insert(main_order.end(), c->begin(), c->end());
      }
    };

    wto.visit_depth_first(profile_order);
    if (!main_order.empty()) {
      m_count_deferred_blocks.fetch_add(deferred_blocks.size(),
                                        std::memory_order_relaxed);
    }
    main_order.insert(main_order.end(), deferred_blocks.begin(),
                      deferred_blocks.end());
    m_count_total_blocks.fetch_add(main_order.size(),
                                   std::memory_order_relaxed);
    return main_order;
  }

  int num_deferred_blocks() { return m_count_deferred_blocks.load(); }
  int num_total_blocks() { return m_count_total_blocks.load(); }

 private:
  const BasicBlockReorderingPass::Config& m_config;
  std::atomic<int> m_count_deferred_blocks{0};
  std::atomic<int> m_count_total_blocks{0};
};
} // namespace

void BasicBlockReorderingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* conf */,
                                        PassManager& mgr) {
  auto scope = build_class_scope(stores);
  std::unique_ptr<cfg::LinearizationStrategy> custom_order =
      std::make_unique<ProfileGuidedLinearization>(m_config);

  // I really hate that I have to do the following. However, this is necessary
  // for me to be able to use both polymorphism and the object's specific
  // methods at the same time. C++ provides no way to define type parameter
  // variances, hence the compiler doesn't know A <: B would imply unique_ptr<A>
  // <: unique_ptr<B>.
  ProfileGuidedLinearization* pgl =
      static_cast<ProfileGuidedLinearization*>(custom_order.get());
  std::atomic<int> num_skipped_methods{0};
  walk::parallel::methods(
      scope, [&custom_order, &num_skipped_methods](DexMethod* m) {
        auto code = m->get_code();
        if (code) {
          bool has_src_blocks = false;
          for (const auto& mie : *code) {
            if (mie.type == MFLOW_SOURCE_BLOCK) {
              has_src_blocks = true;
              break;
            }
          }
          if (has_src_blocks) {
            code->build_cfg(/* editable */ true);
            code->clear_cfg(custom_order);
          } else {
            num_skipped_methods.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
  mgr.incr_metric(METRIC_NUM_BLOCKS_DEFERRED, pgl->num_deferred_blocks());
  mgr.incr_metric(METRIC_NUM_TOTAL_BLOCKS, pgl->num_total_blocks());
  mgr.incr_metric(METRIC_NUM_SKIPPED_METHODS, num_skipped_methods.load());
}

static BasicBlockReorderingPass s_pass;
