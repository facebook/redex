/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <boost/optional.hpp>
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
#include "S_Expression.h"
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

    const std::vector<boost::optional<float>> m_hits;
    uint32_t m_regs{0};
    uint32_t m_insns{0};
    uint32_t m_blocks{0};
    uint32_t m_edges{0};
    uint32_t m_num_loops{0};
    uint32_t m_deepest_loop{0};

   private:
    MethodContext(const MethodContextContext& context,
                  std::vector<boost::optional<float>>&& hits)
        : m_context(context), m_hits(std::move(hits)) {}

    friend class MethodContextContext;
    friend struct RandomForestTestHelper;
  };

  explicit MethodContextContext(const MethodProfiles* profiles)
      : m_interaction_list(create_interaction_list(profiles)),
        m_profiles(profiles) {}

  MethodContext create(const DexMethod* m) {
    std::vector<boost::optional<float>> hits;
    for (const auto& i : m_interaction_list) {
      auto maybe_stat = m_profiles->get_method_stat(i, m);
      if (maybe_stat) {
        hits.emplace_back(maybe_stat->call_count);
      } else {
        hits.push_back(boost::none);
      }
    }

    MethodContext res{*this, std::move(hits)};

    using namespace cfg;

    auto code = const_cast<DexMethod*>(m)->get_code();
    ScopedCFG cfg(code);

    res.m_regs = cfg->get_registers_size();
    res.m_insns = code->count_opcodes();
    res.m_blocks = cfg->num_blocks();
    res.m_edges = 0; // cfg->num_edges();

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

class DecisionTreeNode {
 public:
  virtual ~DecisionTreeNode() {}

  virtual bool accept(const MethodContext& caller,
                      const MethodContext& callee) const = 0;
  virtual std::unique_ptr<DecisionTreeNode> clone() const = 0;
  virtual std::string dump() const = 0; // Meant for testing.
};

struct DecisionTreeCategory : public DecisionTreeNode {
  bool acc;

  DecisionTreeCategory(bool acc) : acc(acc) {}

  bool accept(const MethodContext&, const MethodContext&) const override {
    return acc;
  }

  std::unique_ptr<DecisionTreeNode> clone() const override {
    return std::make_unique<DecisionTreeCategory>(acc);
  }

  std::string dump() const override {
    return std::string("(acc ") + std::to_string(acc) + ")";
  }
};

struct DecisionTreeFeature : public DecisionTreeNode {
  std::unique_ptr<DecisionTreeNode> true_branch;
  std::unique_ptr<DecisionTreeNode> false_branch;
  std::string feature_name; // For dumping only.
  using FeatureFn =
      std::function<float(const MethodContext&, const MethodContext&)>;
  // Note: for simplicity, feature nodes copy the functions, so that a given
  //       FeatureFunctionMap may go out of scope after a call to deserialize.
  const FeatureFn feature_fn;
  float threshold;

  DecisionTreeFeature(std::unique_ptr<DecisionTreeNode> true_branch,
                      std::unique_ptr<DecisionTreeNode> false_branch,
                      std::string feature_name,
                      const FeatureFn& fn,
                      float threshold)
      : true_branch(std::move(true_branch)),
        false_branch(std::move(false_branch)),
        feature_name(std::move(feature_name)),
        feature_fn(fn),
        threshold(threshold) {}

  bool accept(const MethodContext& caller,
              const MethodContext& callee) const override {
    return feature_fn(caller, callee) <= threshold
               ? true_branch->accept(caller, callee)
               : false_branch->accept(caller, callee);
  }

  std::unique_ptr<DecisionTreeNode> clone() const override {
    return std::make_unique<DecisionTreeFeature>(
        true_branch->clone(), false_branch->clone(), feature_name, feature_fn,
        threshold);
  }

  std::string dump() const override {
    return std::string("(feat \"") + feature_name + "\" " +
           std::to_string(threshold) + " " + true_branch->dump() + " " +
           false_branch->dump() + ")";
  }
};

inline float get_max_hits_or_zero(const MethodContext& context) {
  boost::optional<float> max = boost::none;
  for (const auto& v : context.m_hits) {
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

using FeatureFunctionMap =
    std::unordered_map<std::string, DecisionTreeFeature::FeatureFn>;

inline FeatureFunctionMap get_default_feature_function_map() {
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

using namespace sparta;

inline std::unique_ptr<DecisionTreeNode> deserialize_tree(
    const s_expr& expr, const FeatureFunctionMap& feature_fns) {
  s_expr tail;
  if (s_patn({s_patn("acc")}, tail).match_with(expr)) {
    always_assert(tail.size() == 2);
    std::string acc_str, rej_str;
    s_expr rest;
    s_patn({s_patn(&acc_str), s_patn(&rej_str)}, rest)
        .must_match(tail, "Need acc and rej count");
    always_assert(rest.is_nil());
    size_t idx;
    size_t acc = std::stoul(acc_str, &idx);
    always_assert(idx == acc_str.length());
    size_t rej = std::stoul(rej_str, &idx);
    always_assert(idx == rej_str.length());
    always_assert(acc != 0 || rej != 0);
    return std::make_unique<DecisionTreeCategory>(acc >= rej);
  }

  s_patn({s_patn("feat")}, tail).must_match(expr, "Expected feat or acc");
  always_assert(tail.size() == 4);
  std::string feature;
  std::string threshold_str;
  s_expr rest;
  s_patn({s_patn(&feature), s_patn(&threshold_str)}, rest)
      .must_match(tail, "Expected feature format");

  size_t idx;
  float threshold = std::stof(threshold_str, &idx);
  always_assert(idx == threshold_str.length());
  always_assert(threshold >= 0);

  auto fn_it = feature_fns.find(feature);
  always_assert_log(fn_it != feature_fns.end(), "%s", feature.c_str());

  auto lhs = deserialize_tree(rest[0], feature_fns);
  auto rhs = deserialize_tree(rest[1], feature_fns);

  return std::make_unique<DecisionTreeFeature>(
      std::move(lhs), std::move(rhs), feature, fn_it->second, threshold);
}

class Forest {
 public:
  Forest() = default;
  Forest(Forest&&) = default;
  Forest& operator=(Forest&& other) = default;

  // Do not enable copy semantics to avoid accidental expensive copies.
  Forest clone() const {
    Forest ret;
    ret.m_trees.reserve(m_trees.size());
    std::transform(m_trees.begin(), m_trees.end(),
                   std::back_inserter(ret.m_trees),
                   [](const auto& tree) { return tree->clone(); });
    return ret;
  }

  // Note: for simplicity, feature nodes copy the functions, so that a given
  //       FeatureFunctionMap may go out of scope after a call to deserialize.
  static Forest deserialize(const std::string& serialized_forest,
                            const FeatureFunctionMap& feature_fns =
                                get_default_feature_function_map()) {
    std::istringstream input(serialized_forest);
    s_expr_istream s_expr_input(input);
    s_expr expr;
    while (s_expr_input.good()) {
      s_expr_input >> expr;
      if (s_expr_input.eoi()) {
        break;
      }
      always_assert_log(!s_expr_input.fail(), "%s\n",
                        s_expr_input.what().c_str());
    }

    s_expr trees_expr;
    int32_t tree_count;
    s_patn({s_patn("forest")}, trees_expr)
        .must_match(expr, "Missing forest tag");
    always_assert(trees_expr.size() > 0);

    Forest ret;
    ret.m_trees.reserve(trees_expr.size());
    for (size_t i = 0; i < trees_expr.size(); ++i) {
      TRACE(METH_PROF, 5, "Parsing tree %zu", i);
      ret.m_trees.emplace_back(deserialize_tree(trees_expr[i], feature_fns));
    }
    return ret;
  }

  size_t size() const { return m_trees.size(); }

  bool accept(const MethodContext& caller, const MethodContext& callee) const {
    size_t acc_count{0};
    for (const auto& dec_tree : m_trees) {
      if (dec_tree->accept(caller, callee)) {
        ++acc_count;
      }
    }
    return 2 * acc_count >= m_trees.size();
  }

  std::string dump() const {
    std::ostringstream oss;
    bool first = true;
    for (const auto& tree : m_trees) {
      if (!first) {
        oss << "\n";
      }
      first = false;
      oss << tree->dump();
    }
    return oss.str();
  }

 private:
  std::vector<std::unique_ptr<DecisionTreeNode>> m_trees;
};

} // namespace random_forest
