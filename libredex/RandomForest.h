/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "Debug.h"
#include "S_Expression.h"
#include "Trace.h"

// A simple random forest implementation for inlining decisions. The structure
// and types are specialized for that use case. The header only exists for
// testability.

namespace random_forest {

using namespace sparta;

template <typename... Args>
class Forest {
 public:
  class DecisionTreeNode {
   public:
    virtual ~DecisionTreeNode() {}

    virtual float accept(Args... args) const = 0;
    virtual std::unique_ptr<DecisionTreeNode> clone() const = 0;
    virtual std::string dump() const = 0; // Meant for testing.
  };

  struct DecisionTreeCategory : public DecisionTreeNode {
    float acc;

    explicit DecisionTreeCategory(float acc) : acc(acc) {}

    float accept(Args...) const override { return acc; }

    std::unique_ptr<DecisionTreeNode> clone() const override {
      return std::make_unique<DecisionTreeCategory>(acc);
    }

    std::string dump() const override {
      return std::string("(accf ") + std::to_string(acc) + ")";
    }
  };

  struct DecisionTreeFeature : public DecisionTreeNode {
    std::unique_ptr<DecisionTreeNode> true_branch;
    std::unique_ptr<DecisionTreeNode> false_branch;
    std::string feature_name; // For dumping only.
    using FeatureFn = std::function<float(Args...)>;
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

    float accept(Args... args) const override {
      return feature_fn(args...) <= threshold ? true_branch->accept(args...)
                                              : false_branch->accept(args...);
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

  using FeatureFunctionMap =
      std::unordered_map<std::string, typename DecisionTreeFeature::FeatureFn>;

  static std::unique_ptr<DecisionTreeNode> deserialize_tree(
      const s_expr& expr, const FeatureFunctionMap& feature_fns) {
    s_expr tail;
    // Old boolean style
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
      return std::make_unique<DecisionTreeCategory>(acc >= rej ? 1.0 : 0.0);
    }

    if (s_patn({s_patn("accf")}, tail).match_with(expr)) {
      always_assert(tail.size() == 1);
      std::string acc_str;
      s_expr rest;
      s_patn({s_patn(&acc_str)}, rest).must_match(tail, "Need acc value");
      always_assert(rest.is_nil());
      size_t idx;
      auto acc = std::stof(acc_str, &idx);
      always_assert(idx == acc_str.length());
      return std::make_unique<DecisionTreeCategory>(acc);
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

 public:
  Forest() = default;
  Forest(Forest&&) noexcept = default;
  Forest& operator=(Forest&& other) noexcept = default;

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
                            const FeatureFunctionMap& feature_fns) {
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

  bool accept(Args... args, float* c = nullptr) const {
    float acc_sum{0};
    for (const auto& dec_tree : m_trees) {
      acc_sum += dec_tree->accept(args...);
    }
    if (c != nullptr) {
      *c = acc_sum;
    }
    return 2 * acc_sum >= m_trees.size();
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
