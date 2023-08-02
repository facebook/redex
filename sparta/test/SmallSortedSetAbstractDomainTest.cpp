/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/SmallSortedSetAbstractDomain.h>

#include <gmock/gmock.h>

using namespace sparta;

using Set = FlatSet<unsigned>;
using Domain = SmallSortedSetAbstractDomain<unsigned, /* MaxCount */ 4>;

class SmallSortedSetAbstractDomainTest : public ::testing::Test {};

TEST_F(SmallSortedSetAbstractDomainTest, constructor) {
  EXPECT_TRUE(Domain().is_value());
  EXPECT_TRUE(Domain().empty());

  EXPECT_TRUE(Domain(1).is_value());
  EXPECT_EQ(Domain(1).elements(), Set{1});

  EXPECT_TRUE((Domain{1, 2, 3, 4}).is_value());
  EXPECT_EQ((Domain{1, 2, 3, 4}).elements(), (Set{1, 2, 3, 4}));

  EXPECT_TRUE((Domain{1, 2, 3, 4, 5}).is_top());
}

TEST_F(SmallSortedSetAbstractDomainTest, leq) {
  EXPECT_TRUE(Domain::bottom().leq(Domain::bottom()));
  EXPECT_TRUE(Domain::bottom().leq(Domain()));
  EXPECT_TRUE(Domain::bottom().leq(Domain(1)));
  EXPECT_TRUE(Domain::bottom().leq(Domain::top()));

  EXPECT_FALSE(Domain::top().leq(Domain::bottom()));
  EXPECT_FALSE(Domain::top().leq(Domain()));
  EXPECT_FALSE(Domain::top().leq(Domain(1)));
  EXPECT_TRUE(Domain::top().leq(Domain::top()));

  EXPECT_FALSE(Domain().leq(Domain::bottom()));
  EXPECT_TRUE(Domain().leq(Domain()));
  EXPECT_TRUE(Domain().leq(Domain(1)));
  EXPECT_TRUE(Domain().leq(Domain::top()));

  EXPECT_FALSE(Domain(1).leq(Domain::bottom()));
  EXPECT_FALSE(Domain(1).leq(Domain()));
  EXPECT_TRUE(Domain(1).leq(Domain(1)));
  EXPECT_TRUE(Domain(1).leq(Domain::top()));

  EXPECT_TRUE(Domain{1}.leq(Domain{1}));
  EXPECT_FALSE(Domain{1}.leq(Domain{2}));
  EXPECT_TRUE((Domain{1}).leq(Domain{1, 2}));
  EXPECT_FALSE((Domain{1, 2}).leq(Domain{1}));
  EXPECT_TRUE((Domain{1, 3}).leq(Domain{1, 2, 3}));
  EXPECT_FALSE((Domain{1, 2, 3}).leq(Domain{1, 3}));
}

TEST_F(SmallSortedSetAbstractDomainTest, equals) {
  EXPECT_TRUE(Domain::bottom().equals(Domain::bottom()));
  EXPECT_FALSE(Domain::bottom().equals(Domain()));
  EXPECT_FALSE(Domain::bottom().equals(Domain(1)));
  EXPECT_FALSE(Domain::bottom().equals(Domain::top()));

  EXPECT_FALSE(Domain::top().equals(Domain::bottom()));
  EXPECT_FALSE(Domain::top().equals(Domain()));
  EXPECT_FALSE(Domain::top().equals(Domain(1)));
  EXPECT_TRUE(Domain::top().equals(Domain::top()));

  EXPECT_FALSE(Domain().equals(Domain::bottom()));
  EXPECT_TRUE(Domain().equals(Domain()));
  EXPECT_FALSE(Domain().equals(Domain(1)));
  EXPECT_FALSE(Domain().equals(Domain::top()));

  EXPECT_FALSE(Domain(1).equals(Domain::bottom()));
  EXPECT_FALSE(Domain(1).equals(Domain()));
  EXPECT_TRUE(Domain(1).equals(Domain(1)));
  EXPECT_FALSE(Domain(1).equals(Domain::top()));

  EXPECT_TRUE(Domain{1}.equals(Domain{1}));
  EXPECT_FALSE(Domain{1}.equals(Domain{2}));
  EXPECT_FALSE((Domain{1}).equals(Domain{1, 2}));
  EXPECT_FALSE((Domain{1, 2}).equals(Domain{1}));
  EXPECT_TRUE((Domain{1, 2}).equals(Domain{2, 1}));
  EXPECT_FALSE((Domain{1, 3}).equals(Domain{1, 2, 3}));
  EXPECT_FALSE((Domain{1, 2, 3}).equals(Domain{1, 3}));
}

TEST_F(SmallSortedSetAbstractDomainTest, join) {
  EXPECT_EQ(Domain::bottom().join(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain::bottom().join(Domain()), Domain());
  EXPECT_EQ(Domain::bottom().join(Domain(1)), Domain(1));
  EXPECT_EQ(Domain::bottom().join(Domain::top()), Domain::top());

  EXPECT_EQ(Domain::top().join(Domain::bottom()), Domain::top());
  EXPECT_EQ(Domain::top().join(Domain()), Domain::top());
  EXPECT_EQ(Domain::top().join(Domain(1)), Domain::top());
  EXPECT_EQ(Domain::top().join(Domain::top()), Domain::top());

  EXPECT_EQ(Domain().join(Domain::bottom()), Domain());
  EXPECT_EQ(Domain().join(Domain()), Domain());
  EXPECT_EQ(Domain().join(Domain(1)), Domain(1));
  EXPECT_EQ(Domain().join(Domain::top()), Domain::top());

  EXPECT_EQ(Domain{1}.join(Domain{1}), Domain{1});
  EXPECT_EQ(Domain{1}.join(Domain{2}), (Domain{1, 2}));
  EXPECT_EQ((Domain{1}).join(Domain{1, 2}), (Domain{1, 2}));
  EXPECT_EQ((Domain{1, 2}).join(Domain{1}), (Domain{1, 2}));
  EXPECT_EQ((Domain{1, 3}).join(Domain{1, 2, 3}), (Domain{1, 2, 3}));
  EXPECT_EQ((Domain{1, 2, 3}).join(Domain{1, 3}), (Domain{1, 2, 3}));
  EXPECT_EQ((Domain{1, 2, 3}).join(Domain{4}), (Domain{1, 2, 3, 4}));
  EXPECT_EQ((Domain{1, 2}).join(Domain{3, 4, 5}), Domain::top());
  EXPECT_EQ((Domain{1, 2, 3}).join(Domain{4, 5}), Domain::top());
}

TEST_F(SmallSortedSetAbstractDomainTest, meet) {
  EXPECT_EQ(Domain::bottom().meet(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain::bottom().meet(Domain()), Domain::bottom());
  EXPECT_EQ(Domain::bottom().meet(Domain(1)), Domain::bottom());
  EXPECT_EQ(Domain::bottom().meet(Domain::top()), Domain::bottom());

  EXPECT_EQ(Domain::top().meet(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain::top().meet(Domain()), Domain());
  EXPECT_EQ(Domain::top().meet(Domain(1)), Domain(1));
  EXPECT_EQ(Domain::top().meet(Domain::top()), Domain::top());

  EXPECT_EQ(Domain().meet(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain().meet(Domain()), Domain());
  EXPECT_EQ(Domain().meet(Domain(1)), Domain());
  EXPECT_EQ(Domain().meet(Domain::top()), Domain());

  EXPECT_EQ(Domain{1}.meet(Domain{1}), Domain{1});
  EXPECT_EQ(Domain{1}.meet(Domain{2}), Domain());
  EXPECT_EQ((Domain{1}).meet(Domain{1, 2}), Domain{1});
  EXPECT_EQ((Domain{1, 2}).meet(Domain{1}), Domain{1});
  EXPECT_EQ((Domain{1, 3}).meet(Domain{1, 2, 3}), (Domain{1, 3}));
  EXPECT_EQ((Domain{1, 2, 3}).meet(Domain{1, 3}), (Domain{1, 3}));
}

TEST_F(SmallSortedSetAbstractDomainTest, add) {
  auto set = Domain::bottom();
  set.add(1);
  EXPECT_EQ(set, Domain::bottom());

  set = Domain();
  set.add(1);
  EXPECT_EQ(set, Domain{1});

  set = Domain::top();
  set.add(1);
  EXPECT_EQ(set, Domain::top());

  set = Domain{1};
  set.add(1);
  EXPECT_EQ(set, Domain{1});

  set = Domain{1, 2};
  set.add(3);
  EXPECT_EQ(set, (Domain{1, 2, 3}));

  set = Domain{1, 2, 3, 4};
  set.add(1);
  EXPECT_EQ(set, (Domain{1, 2, 3, 4}));

  set = Domain{1, 2, 3, 4};
  set.add(5);
  EXPECT_EQ(set, Domain::top());
}

TEST_F(SmallSortedSetAbstractDomainTest, remove) {
  auto set = Domain::bottom();
  set.remove(1);
  EXPECT_EQ(set, Domain::bottom());

  set = Domain();
  set.remove(1);
  EXPECT_EQ(set, Domain());

  set = Domain::top();
  set.remove(1);
  EXPECT_EQ(set, Domain::top());

  set = Domain{1};
  set.remove(1);
  EXPECT_EQ(set, Domain());

  set = Domain{1, 2};
  set.remove(3);
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1, 2, 3, 4};
  set.remove(1);
  EXPECT_EQ(set, (Domain{2, 3, 4}));
}

TEST_F(SmallSortedSetAbstractDomainTest, contains) {
  EXPECT_FALSE(Domain::bottom().contains(1));
  EXPECT_FALSE(Domain().contains(1));
  EXPECT_TRUE(Domain::top().contains(1));
  EXPECT_TRUE(Domain(1).contains(1));
  EXPECT_TRUE((Domain{1, 2}).contains(1));
  EXPECT_TRUE((Domain{1, 2}).contains(2));
  EXPECT_FALSE((Domain{1, 2}).contains(3));
}
