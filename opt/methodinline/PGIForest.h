/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "IRCode.h"
#include "LoopInfo.h"
#include "MethodProfiles.h"
#include "RandomForest.h"
#include "ScopedCFG.h"
#include "Trace.h"

// A simple random forest implementation for inlining decisions. The structure
// and types are specialized for that use case. The header only exists for
// testability.

namespace random_forest {

using namespace method_profiles;

// The context for MethodContext classes. The context exists so that the
// internal structure is ordered and does not need a map. Specifically,
// the MethodContextContext provides an ordering of interactions, which
// is used for the `hits` of a method.
class MethodContextContext {
 public:
  const std::vector<std::string> m_interaction_list;

  // Actual data for a method.
  class MethodContext {
   public:
    const MethodContextContext& m_context;

    struct Vals {
      std::vector<boost::optional<float>> hits;
      std::vector<boost::optional<float>> appear100;
    };
    const boost::optional<Vals> m_vals;

    uint32_t m_regs{0};
    uint32_t m_insns{0};
    uint32_t m_blocks{0};
    uint32_t m_edges{0};
    uint32_t m_num_loops{0};
    uint32_t m_deepest_loop{0};

   private:
    MethodContext(const MethodContextContext& context,
                  boost::optional<Vals>&& vals)
        : m_context(context), m_vals(std::move(vals)) {}

    friend class MethodContextContext;
    friend struct RandomForestTestHelper;
  };

  explicit MethodContextContext(const MethodProfiles* profiles)
      : m_interaction_list(create_interaction_list(profiles)),
        m_profiles(profiles) {}

  MethodContext create(const DexMethod* m) {
    std::vector<boost::optional<float>> hits;
    std::vector<boost::optional<float>> appear;
    bool has_data = false;
    boost::optional<MethodContext::Vals> vals = boost::none;
    for (const auto& i : m_interaction_list) {
      auto maybe_stat = m_profiles->get_method_stat(i, m);
      if (maybe_stat) {
        has_data = true;
        hits.emplace_back(maybe_stat->call_count);
        appear.emplace_back(maybe_stat->appear_percent);
      } else {
        hits.push_back(boost::none);
        appear.push_back(boost::none);
      }
    }
    if (has_data) {
      vals = boost::make_optional(MethodContext::Vals({hits, appear}));
    }

    MethodContext res{*this, std::move(vals)};

    using namespace cfg;

    auto code = const_cast<DexMethod*>(m)->get_code();
    ScopedCFG cfg(code);

    res.m_regs = cfg->get_registers_size();
    res.m_insns = code->count_opcodes();
    res.m_blocks = cfg->num_blocks();
    res.m_edges = cfg->num_edges();

    // Somewhat expensive.
    loop_impl::LoopInfo info(*cfg);
    res.m_num_loops = info.num_loops();
    for (auto* loop : info) {
      res.m_deepest_loop =
          std::max(res.m_deepest_loop, (uint32_t)loop->get_loop_depth());
    }

    return res;
  }

 private:
  MethodContextContext() : m_interaction_list() {}

  static std::vector<std::string> create_interaction_list(
      const MethodProfiles* profiles) {
    std::vector<std::string> res;
    for (const auto& stat_p : profiles->all_interactions()) {
      res.push_back(stat_p.first);
    }
    std::sort(res.begin(), res.end());
    return res;
  }

  const MethodProfiles* m_profiles{nullptr};

  friend struct RandomForestTestHelper;
};

using MethodContext = MethodContextContext::MethodContext;

using PGIForest = Forest<const MethodContext&, const MethodContext&>;

inline float get_max_hits_or_zero(const MethodContext& context) {
  if (!context.m_vals) {
    return 0;
  }
  boost::optional<float> max = boost::none;
  for (const auto& v : context.m_vals->hits) {
    if (v) {
      if (!max) {
        max = v;
      } else {
        max = std::max(*max, *v);
      }
    }
  }
  if (max) {
    return *max;
  }
  return 0;
}

inline PGIForest::FeatureFunctionMap get_default_feature_function_map() {
  return {
      // Caller.
      {"caller_hits",
       [](const MethodContext& caller, const MethodContext&) {
         return get_max_hits_or_zero(caller);
       }},
      {"caller_insns", [](const MethodContext& caller,
                          const MethodContext&) { return caller.m_insns; }},
      {"caller_regs", [](const MethodContext& caller,
                         const MethodContext&) { return caller.m_regs; }},
      {"caller_blocks", [](const MethodContext& caller,
                           const MethodContext&) { return caller.m_blocks; }},
      {"caller_edges", [](const MethodContext& caller,
                          const MethodContext&) { return caller.m_edges; }},
      {"caller_num_loops",
       [](const MethodContext& caller, const MethodContext&) {
         return caller.m_num_loops;
       }},
      {"caller_deepest_loop",
       [](const MethodContext& caller, const MethodContext&) {
         return caller.m_deepest_loop;
       }},
      // Callee.
      {"callee_hits",
       [](const MethodContext&, const MethodContext& callee) {
         return get_max_hits_or_zero(callee);
       }},
      {"callee_insns",
       [](const MethodContext&, const MethodContext& callee) {
         return callee.m_insns;
       }},
      {"callee_regs",
       [](const MethodContext&, const MethodContext& callee) {
         return callee.m_regs;
       }},
      {"callee_blocks",
       [](const MethodContext&, const MethodContext& callee) {
         return callee.m_blocks;
       }},
      {"callee_edges",
       [](const MethodContext&, const MethodContext& callee) {
         return callee.m_edges;
       }},
      {"callee_num_loops",
       [](const MethodContext&, const MethodContext& callee) {
         return callee.m_num_loops;
       }},
      {"callee_deepest_loop",
       [](const MethodContext&, const MethodContext& callee) {
         return callee.m_deepest_loop;
       }},
  };
}

} // namespace random_forest
