/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IntervalDomain.h"

#include <gtest/gtest.h>
#include <limits>
#include <sstream>

using namespace sparta;

namespace {

using Domain = IntervalDomain<int>;

TEST(IntervalDomainTest, top) {
  EXPECT_TRUE(Domain::top().is_top());

  const Domain i_top;
  EXPECT_TRUE(i_top.is_top());
}

TEST(IntervalDomainTest, bottom) { EXPECT_TRUE(Domain::bottom().is_bottom()); }

TEST(IntervalDomainTest, addition) {
  const auto a = Domain::finite(-7, 5);
  const auto b = Domain::finite(-3, 5);
  const auto bot = Domain::bottom();

  EXPECT_EQ(a + b, Domain::finite(-10, 10));
  EXPECT_EQ(a + bot, bot);
  EXPECT_EQ(bot + b, bot);
}

TEST(IntervalDomainTest, saturatedAddition) {
  const auto top = Domain::top();
  const auto high = Domain::high();
  const auto low = Domain::low();

  const auto pp = Domain::finite(+1, +1);
  const auto np = Domain::finite(-1, +1);
  const auto nn = Domain::finite(-1, -1);

  EXPECT_EQ(top + pp, top);
  EXPECT_EQ(top + np, top);
  EXPECT_EQ(top + nn, top);

  EXPECT_EQ(high + pp, high);
  EXPECT_EQ(high + np, Domain::bounded_below(Domain::MAX - 1));
  EXPECT_EQ(high + nn, Domain::bounded_below(Domain::MAX - 1));

  EXPECT_EQ(low + pp, Domain::bounded_above(Domain::MIN + 1));
  EXPECT_EQ(low + np, Domain::bounded_above(Domain::MIN + 1));
  EXPECT_EQ(low + nn, low);

  auto pos = Domain::bounded_below(+1);
  auto neg = Domain::bounded_above(-1);

  pos += 1;
  EXPECT_EQ(pos, Domain::bounded_below(+2));
  pos += -1;
  EXPECT_EQ(pos, Domain::bounded_below(+1));

  neg += -1;
  EXPECT_EQ(neg, Domain::bounded_above(-2));
  neg += +1;
  EXPECT_EQ(neg, Domain::bounded_above(-1));
}

TEST(IntervalDomainTest, ordering) {
  const auto a = Domain::finite(-5, 5);
  const auto b = Domain::finite(0, 10);
  const auto c = Domain::bounded_above(5);
  const auto d = Domain::bounded_below(-5);

  const auto bot = Domain::bottom();
  const auto high = Domain::high();
  const auto low = Domain::low();
  const auto top = Domain::top();

  // Bottom is less than everything
  EXPECT_TRUE(bot.leq(a));
  EXPECT_TRUE(bot.leq(high));
  EXPECT_TRUE(bot.leq(top));

  // Nothing is less than bottom
  EXPECT_FALSE(b.leq(bot));
  EXPECT_FALSE(high.leq(bot));
  EXPECT_FALSE(top.leq(bot));

  // Everything is less than top
  EXPECT_TRUE(b.leq(top));
  EXPECT_TRUE(high.leq(top));

  // Everything else
  EXPECT_TRUE(a.leq(c));
  EXPECT_TRUE(a.leq(d));
  EXPECT_TRUE(b.leq(d));

  EXPECT_FALSE(a.leq(b));
  EXPECT_FALSE(b.leq(a));
  EXPECT_FALSE(b.leq(c));

  EXPECT_TRUE(low.leq(c));
  EXPECT_TRUE(high.leq(d));
}

TEST(IntervalDomainTest, lattice) {
  const auto top = Domain::top();
  const auto bot = Domain::bottom();

  const auto a = Domain::finite(-4, 4);
  const auto b = Domain::bounded_below(0);
  const auto c = Domain::bounded_above(-1);
  const auto d = Domain::finite(0, 5);
  const auto e = Domain::finite(-5, -1);

  // Meets and Joins
  EXPECT_EQ(a.join(b), Domain::bounded_below(-4));
  EXPECT_EQ(a.meet(b), Domain::finite(0, 4));

  EXPECT_EQ(b.join(c), top);
  EXPECT_EQ(b.meet(c), bot);

  EXPECT_EQ(a.join(top), top);
  EXPECT_EQ(a.meet(top), a);
  EXPECT_EQ(a.join(bot), a);
  EXPECT_EQ(a.meet(bot), bot);

  // Widening
  EXPECT_EQ(a.widening(bot), a);
  EXPECT_EQ(bot.widening(a), a);

  EXPECT_EQ(a.widening(d), Domain::bounded_below(-4));
  EXPECT_EQ(a.widening(e), Domain::bounded_above(4));
  EXPECT_EQ(a.widening(d).widening(e), top);

  // Narrowing
  EXPECT_EQ(a.narrowing(bot), bot);
  EXPECT_EQ(bot.narrowing(a), bot);

  EXPECT_EQ(top.narrowing(b), b);
  EXPECT_EQ(top.narrowing(b).narrowing(c), bot);
  EXPECT_EQ(top.narrowing(b).narrowing(a), Domain::finite(0, 4));
}

} // namespace
