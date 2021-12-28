/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LiftedDomain.h"
#include "ConstantAbstractDomain.h"

#include <gtest/gtest.h>

using namespace sparta;

namespace {

// Simple "diamond" shaped underlying domain:
using Underlying = ConstantAbstractDomain<bool>;
using Domain = LiftedDomain<Underlying>;

TEST(LiftedDomainTest, top) {
  EXPECT_TRUE(Domain::top().is_top());

  const Domain i_top;
  EXPECT_TRUE(i_top.is_top());
}

TEST(LiftedDomainTest, bottom) { EXPECT_TRUE(Domain::bottom().is_bottom()); }

TEST(LiftedDomainTest, ordering) {
  const auto bot = Domain::bottom();
  const auto top = Domain::top();
  const auto lbot = Domain::lifted(Underlying::bottom());
  const auto t = Domain::lifted(Underlying(true));
  const auto f = Domain::lifted(Underlying(false));

  // Bottom is less than everything.
  EXPECT_TRUE(bot.leq(lbot));
  EXPECT_TRUE(bot.leq(t));
  EXPECT_TRUE(bot.leq(f));
  EXPECT_TRUE(bot.leq(top));

  // Lifted bottom is still less than everything except bottom.
  EXPECT_TRUE(lbot.leq(t));
  EXPECT_TRUE(lbot.leq(f));
  EXPECT_TRUE(lbot.leq(top));
}

TEST(LiftedDomainTest, meetAndJoin) {
  const auto bot = Domain::bottom();
  const auto lbot = Domain::lifted(Underlying::bottom());

  EXPECT_EQ(bot.join(lbot), lbot);
  EXPECT_EQ(bot.meet(lbot), bot);
}

} // namespace
