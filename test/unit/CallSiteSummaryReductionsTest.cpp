/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CallSiteSummaryReductions.h"
#include "RedexTest.h"

using namespace partial_application;

namespace {

// Builds a call-site summary binding the given (argument index, value) pairs.
CallSiteSummary make_css(
    const std::vector<std::pair<param_index_t, int64_t>>& bindings) {
  CallSiteSummary css;
  css.result_used = false;
  for (const auto& [idx, value] : bindings) {
    css.arguments.set(idx, SignedConstantDomain(value));
  }
  return css;
}

} // namespace

class CallSiteSummaryReductionsTest : public RedexTest {};

TEST_F(CallSiteSummaryReductionsTest, is_reduction_of) {
  auto none = make_css({});
  auto a = make_css({{0, 2}});
  auto ab = make_css({{0, 2}, {1, 2}});
  auto b = make_css({{1, 2}});
  auto a_other = make_css({{0, 3}});

  // Dropping bindings is fine; adding or changing them is not.
  EXPECT_TRUE(is_reduction_of(&a, &a));
  EXPECT_TRUE(is_reduction_of(&a, &ab));
  EXPECT_TRUE(is_reduction_of(&b, &ab));
  EXPECT_TRUE(is_reduction_of(&none, &ab));
  EXPECT_FALSE(is_reduction_of(&ab, &a));
  EXPECT_FALSE(is_reduction_of(&a, &b));
  EXPECT_FALSE(is_reduction_of(&a_other, &a));
  EXPECT_FALSE(is_reduction_of(&a, &none));

  // A helper method that returns a result cannot serve a call-site that
  // discards it, and vice versa.
  auto a_result_used = make_css({{0, 2}});
  a_result_used.result_used = true;
  EXPECT_FALSE(is_reduction_of(&a_result_used, &a));
  EXPECT_FALSE(is_reduction_of(&a, &a_result_used));
}

TEST_F(CallSiteSummaryReductionsTest, find_selected_follows_chain) {
  auto abc = make_css({{0, 2}, {1, 2}, {2, 2}});
  auto ab = make_css({{0, 2}, {1, 2}});
  auto a = make_css({{0, 2}});

  CallSiteSummaryReductions reductions;
  reductions.add(&abc, &ab);
  reductions.add(&ab, &a);

  UnorderedSet<const CallSiteSummary*> selected{&a};
  auto is_selected = [&selected](const CallSiteSummary* css) {
    return selected.count(css) != 0u;
  };

  EXPECT_EQ(reductions.find_selected(&abc, is_selected), &a);
  EXPECT_EQ(reductions.find_selected(&ab, is_selected), &a);
  EXPECT_EQ(reductions.find_selected(&a, is_selected), &a);

  // The strongest selected summary on the chain wins, not the weakest.
  selected.insert(&ab);
  EXPECT_EQ(reductions.find_selected(&abc, is_selected), &ab);
  EXPECT_EQ(reductions.find_selected(&ab, is_selected), &ab);
  EXPECT_EQ(reductions.find_selected(&a, is_selected), &a);
}

TEST_F(CallSiteSummaryReductionsTest, find_selected_no_selection) {
  auto ab = make_css({{0, 2}, {1, 2}});
  auto a = make_css({{0, 2}});

  CallSiteSummaryReductions reductions;
  reductions.add(&ab, &a);

  UnorderedSet<const CallSiteSummary*> selected;
  auto is_selected = [&selected](const CallSiteSummary* css) {
    return selected.count(css) != 0u;
  };

  EXPECT_EQ(reductions.find_selected(&ab, is_selected), nullptr);
  EXPECT_EQ(reductions.find_selected(&a, is_selected), nullptr);
}

// A summary can be reduced more than once: after it has been reduced, other
// summaries can transfer their costs onto it, putting it back in the priority
// queue, from where it can be reduced again along a different argument. The
// chain must then follow the latest reduction -- and that is still a valid
// substitute, because every reduction only ever drops bindings.
TEST_F(CallSiteSummaryReductionsTest, find_selected_after_re_reduction) {
  auto ab = make_css({{0, 2}, {1, 2}});
  auto a = make_css({{0, 2}});
  auto b = make_css({{1, 2}});

  CallSiteSummaryReductions reductions;
  reductions.add(&ab, &a);
  reductions.add(&ab, &b);

  UnorderedSet<const CallSiteSummary*> selected{&a, &b};
  auto is_selected = [&selected](const CallSiteSummary* css) {
    return selected.count(css) != 0u;
  };

  const auto* selected_css = reductions.find_selected(&ab, is_selected);
  EXPECT_EQ(selected_css, &b);
  EXPECT_TRUE(is_reduction_of(selected_css, &ab));
}

/*
 * A call-site must never be served by a helper method that binds constants the
 * call-site does not pass.
 *
 * This is the summary/reduction graph CalleeInvocationSelector produces for a
 * three-argument callee whose argument costs make the priority queue drain in
 * this order:
 *
 *   {0:2,1:2}       is reduced to {0:2}
 *   {0:2,2:2}       is reduced to {0:2}
 *   {0:2}           is reduced to nothing, and dropped
 *   {0:2,1:2,2:2}   is reduced to {0:2,1:2}, which had already been reduced
 *                   itself, and which ends up being the selected summary
 *
 * A representation that loses the direction of each reduction -- a set of
 * merged summaries, say -- puts {0:2} and {0:2,1:2} together, and so would
 * serve a call-site passing only `arg0 == 2` from the helper method that also
 * hardcodes `arg1 == 2`.
 */
TEST_F(CallSiteSummaryReductionsTest, does_not_bind_unrelated_constants) {
  auto a = make_css({{0, 2}});
  auto ab = make_css({{0, 2}, {1, 2}});
  auto ac = make_css({{0, 2}, {2, 2}});
  auto abc = make_css({{0, 2}, {1, 2}, {2, 2}});
  auto b = make_css({{1, 2}});

  CallSiteSummaryReductions reductions;
  reductions.add(&ab, &a);
  reductions.add(&ac, &a);
  reductions.add(&abc, &ab);

  UnorderedSet<const CallSiteSummary*> selected{&ab};
  auto is_selected = [&selected](const CallSiteSummary* css) {
    return selected.count(css) != 0u;
  };

  // A call-site that only passes arg0 must not be served by the helper that
  // also binds arg1.
  EXPECT_EQ(reductions.find_selected(&a, is_selected), nullptr);
  EXPECT_EQ(reductions.find_selected(&ac, is_selected), nullptr);
  EXPECT_EQ(reductions.find_selected(&b, is_selected), nullptr);

  // Call-sites that do pass arg0 and arg1 are served.
  EXPECT_EQ(reductions.find_selected(&ab, is_selected), &ab);
  EXPECT_EQ(reductions.find_selected(&abc, is_selected), &ab);

  // Whatever is selected is always a valid substitute for the call-site.
  for (const auto* css : {&a, &ab, &ac, &abc, &b}) {
    const auto* selected_css = reductions.find_selected(css, is_selected);
    if (selected_css != nullptr) {
      EXPECT_TRUE(is_reduction_of(selected_css, css))
          << selected_css->get_key() << " is not a reduction of "
          << css->get_key();
    }
  }
}
