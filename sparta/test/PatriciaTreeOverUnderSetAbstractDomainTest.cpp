/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/PatriciaTreeOverUnderSetAbstractDomain.h>

#include <gmock/gmock.h>

using namespace sparta;

using Set = PatriciaTreeSet<unsigned>;
using Domain = PatriciaTreeOverUnderSetAbstractDomain<unsigned>;

class PatriciaTreeOverUnderSetAbstractDomainTest : public ::testing::Test {};

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, constructor) {
  EXPECT_TRUE(Domain().is_value());
  EXPECT_EQ(Domain(1).over(), Set{});
  EXPECT_EQ(Domain(1).under(), Set{1});
  EXPECT_EQ((Domain{1, 2}.over()), Set{});
  EXPECT_EQ((Domain{1, 2}.under()), (Set{1, 2}));
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{2}).over(), Set{1});
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{2}).under(), Set{2});
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2}).over(), Set{1});
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2}).under(), Set{2});
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, leq) {
  EXPECT_TRUE(Domain::bottom().leq(Domain::bottom()));
  EXPECT_TRUE(Domain::bottom().leq(Domain()));
  EXPECT_TRUE(Domain::bottom().leq(Domain::top()));
  EXPECT_FALSE(Domain::top().leq(Domain::bottom()));
  EXPECT_FALSE(Domain::top().leq(Domain()));
  EXPECT_TRUE(Domain::top().leq(Domain::top()));
  EXPECT_FALSE(Domain().leq(Domain::bottom()));
  EXPECT_TRUE(Domain().leq(Domain()));
  EXPECT_TRUE(Domain().leq(Domain::top()));

  // Test with over = under.
  EXPECT_TRUE(Domain{1}.leq(Domain{1}));
  EXPECT_FALSE(Domain{1}.leq(Domain{2}));
  EXPECT_FALSE((Domain{1}).leq(Domain{1, 2}));
  EXPECT_FALSE((Domain{1, 2}).leq(Domain{1}));
  EXPECT_FALSE((Domain{1, 3}).leq(Domain{1, 2, 3}));
  EXPECT_FALSE((Domain{1, 2, 3}).leq(Domain{1, 3}));

  // Test with under = empty.
  EXPECT_TRUE(Domain(/* over */ Set{1}, /* under */ Set{})
                  .leq(Domain(/* over */ Set{1}, /* under */ Set{})));
  EXPECT_TRUE(Domain(/* over */ Set{1}, /* under */ Set{})
                  .leq(Domain(/* over */ Set{1, 2}, /* under */ Set{})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                   .leq(Domain(/* over */ Set{1}, /* under */ Set{})));
  EXPECT_TRUE(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                  .leq(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})
                   .leq(Domain(/* over */ Set{1, 3}, /* under */ Set{})));

  // Test with under != over.
  EXPECT_TRUE(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                  .leq(Domain(/* over */ Set{1, 2}, /* under */ Set{2})));
  EXPECT_TRUE(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                  .leq(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})));
  EXPECT_FALSE(
      Domain(/* over */ Set{1, 2}, /* under */ Set{2})
          .leq(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})));
  EXPECT_TRUE(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                  .leq(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                   .leq(Domain(/* over */ Set{1, 2}, /* under */ Set{2})));
  EXPECT_FALSE(
      Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
          .leq(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})));
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, equals) {
  EXPECT_TRUE(Domain::bottom().equals(Domain::bottom()));
  EXPECT_FALSE(Domain::bottom().equals(Domain()));
  EXPECT_FALSE(Domain::bottom().equals(Domain::top()));
  EXPECT_FALSE(Domain::top().equals(Domain::bottom()));
  EXPECT_FALSE(Domain::top().equals(Domain()));
  EXPECT_TRUE(Domain::top().equals(Domain::top()));
  EXPECT_FALSE(Domain().equals(Domain::bottom()));
  EXPECT_TRUE(Domain().equals(Domain()));
  EXPECT_FALSE(Domain().equals(Domain::top()));

  // Test with over = under.
  EXPECT_TRUE(Domain{1}.equals(Domain{1}));
  EXPECT_FALSE(Domain{1}.equals(Domain{2}));
  EXPECT_FALSE((Domain{1}).equals(Domain{1, 2}));
  EXPECT_FALSE((Domain{1, 2}).equals(Domain{1}));
  EXPECT_FALSE((Domain{1, 3}).equals(Domain{1, 2, 3}));
  EXPECT_FALSE((Domain{1, 2, 3}).equals(Domain{1, 3}));

  // Test with under = empty.
  EXPECT_TRUE(Domain(/* over */ Set{1}, /* under */ Set{})
                  .equals(Domain(/* over */ Set{1}, /* under */ Set{})));
  EXPECT_FALSE(Domain(/* over */ Set{1}, /* under */ Set{})
                   .equals(Domain(/* over */ Set{1, 2}, /* under */ Set{})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                   .equals(Domain(/* over */ Set{1}, /* under */ Set{})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                   .equals(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})
                   .equals(Domain(/* over */ Set{1, 3}, /* under */ Set{})));

  // Test with under != over.
  EXPECT_TRUE(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                  .equals(Domain(/* over */ Set{1, 2}, /* under */ Set{2})));
  EXPECT_FALSE(
      Domain(/* over */ Set{1, 2}, /* under */ Set{2})
          .equals(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})));
  EXPECT_FALSE(
      Domain(/* over */ Set{1, 2}, /* under */ Set{2})
          .equals(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})));
  EXPECT_FALSE(
      Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
          .equals(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})));
  EXPECT_FALSE(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                   .equals(Domain(/* over */ Set{1, 2}, /* under */ Set{2})));
  EXPECT_FALSE(
      Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
          .equals(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})));
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, join) {
  EXPECT_EQ(Domain::bottom().join(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain::bottom().join(Domain()), Domain());
  EXPECT_EQ(Domain::bottom().join(Domain::top()), Domain::top());
  EXPECT_EQ(Domain::top().join(Domain::bottom()), Domain::top());
  EXPECT_EQ(Domain::top().join(Domain()), Domain::top());
  EXPECT_EQ(Domain::top().join(Domain::top()), Domain::top());
  EXPECT_EQ(Domain().join(Domain::bottom()), Domain());
  EXPECT_EQ(Domain().join(Domain()), Domain());
  EXPECT_EQ(Domain().join(Domain::top()), Domain::top());

  // Test with over = under.
  EXPECT_EQ(Domain{1}.join(Domain{1}), Domain{1});
  EXPECT_EQ(Domain{1}.join(Domain{2}),
            Domain(/* over */ Set{1, 2}, /* under */ {}));
  EXPECT_EQ((Domain{1}).join(Domain{1, 2}),
            Domain(/* over */ Set{1, 2}, /* under */ Set{1}));
  EXPECT_EQ((Domain{1, 2}).join(Domain{1}),
            Domain(/* over */ Set{1, 2}, /* under */ Set{1}));
  EXPECT_EQ((Domain{1, 3}).join(Domain{1, 2, 3}),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 3}));
  EXPECT_EQ((Domain{1, 2, 3}).join(Domain{1, 3}),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 3}));

  // Test with under = empty.
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{})
                .join(Domain(/* over */ Set{1}, /* under */ Set{})),
            Domain(/* over */ Set{1}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{})
                .join(Domain(/* over */ Set{1, 2}, /* under */ Set{})),
            Domain(/* over */ Set{1, 2}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                .join(Domain(/* over */ Set{1}, /* under */ Set{})),
            Domain(/* over */ Set{1, 2}, /* under */ {}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                .join(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})
                .join(Domain(/* over */ Set{1, 3}, /* under */ Set{})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{}));

  // Test with under != over.
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                .join(Domain(/* over */ Set{1, 2}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                .join(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                .join(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                .join(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                .join(Domain(/* over */ Set{1, 2}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                .join(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, meet) {
  EXPECT_EQ(Domain::bottom().meet(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain::bottom().meet(Domain()), Domain::bottom());
  EXPECT_EQ(Domain::bottom().meet(Domain::top()), Domain::bottom());
  EXPECT_EQ(Domain::top().meet(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain::top().meet(Domain()), Domain());
  EXPECT_EQ(Domain::top().meet(Domain::top()), Domain::top());
  EXPECT_EQ(Domain().meet(Domain::bottom()), Domain::bottom());
  EXPECT_EQ(Domain().meet(Domain()), Domain());
  EXPECT_EQ(Domain().meet(Domain::top()), Domain());

  // Test with over = under.
  EXPECT_EQ(Domain{1}.meet(Domain{1}), Domain{1});
  EXPECT_EQ(Domain{1}.meet(Domain{2}), Domain::bottom());
  EXPECT_EQ((Domain{1}).meet(Domain{1, 2}), Domain::bottom());
  EXPECT_EQ((Domain{1, 2}).meet(Domain{1}), Domain::bottom());
  EXPECT_EQ((Domain{1, 3}).meet(Domain{1, 2, 3}), Domain::bottom());
  EXPECT_EQ((Domain{1, 2, 3}).meet(Domain{1, 3}), Domain::bottom());

  // Test with under = empty.
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{})
                .meet(Domain(/* over */ Set{1}, /* under */ Set{})),
            Domain(/* over */ Set{1}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{})
                .meet(Domain(/* over */ Set{1, 2}, /* under */ Set{})),
            Domain(/* over */ Set{1}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                .meet(Domain(/* over */ Set{1}, /* under */ Set{})),
            Domain(/* over */ Set{1}, /* under */ {}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{})
                .meet(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})),
            Domain(/* over */ Set{1, 2}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{})
                .meet(Domain(/* over */ Set{1, 3}, /* under */ Set{})),
            Domain(/* over */ Set{1, 3}, /* under */ Set{}));
  EXPECT_EQ(Domain(/* over */ Set{1}, /* under */ Set{})
                .meet(Domain(/* over */ Set{3}, /* under */ Set{})),
            Domain(/* over */ Set{}, /* under */ Set{}));

  // Test with under != over.
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                .meet(Domain(/* over */ Set{1, 2}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                .meet(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2}, /* under */ Set{2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2}, /* under */ Set{2})
                .meet(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})),
            Domain::bottom());
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                .meet(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                .meet(Domain(/* over */ Set{1, 2}, /* under */ Set{2})),
            Domain(/* over */ Set{1, 2}, /* under */ Set{1, 2}));
  EXPECT_EQ(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2})
                .meet(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3})),
            Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2, 3}));
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, add) {
  auto set = Domain::bottom();
  set.add(Domain::bottom());
  EXPECT_EQ(set, Domain::bottom());

  set = Domain::bottom();
  set.add(Domain());
  EXPECT_EQ(set, Domain());

  set = Domain::bottom();
  set.add(Domain::top());
  EXPECT_EQ(set, Domain::top());

  set = Domain::top();
  set.add(Domain::bottom());
  EXPECT_EQ(set, Domain::top());

  set = Domain::top();
  set.add(Domain());
  EXPECT_EQ(set, Domain::top());

  set = Domain::top();
  set.add(Domain::top());
  EXPECT_EQ(set, Domain::top());

  set = Domain();
  set.add(Domain::bottom());
  EXPECT_EQ(set, Domain());

  set = Domain();
  set.add(Domain());
  EXPECT_EQ(set, Domain());

  set = Domain();
  set.add(Domain::top());
  EXPECT_EQ(set, Domain::top());

  // Test with over = under.
  set = Domain{1};
  set.add(Domain{1});
  EXPECT_EQ(set, Domain{1});

  set = Domain{1};
  set.add(Domain{2});
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1};
  set.add(Domain{1, 2});
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1, 2};
  set.add(Domain{1});
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1, 3};
  set.add(Domain{1, 2, 3});
  EXPECT_EQ(set, (Domain{1, 2, 3}));

  set = Domain{1, 2, 3};
  set.add(Domain{1, 3});
  EXPECT_EQ(set, (Domain{1, 2, 3}));

  // Test with under = empty.
  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add(Domain(/* over */ Set{1}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1}, /* under */ Set{}));

  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add(Domain(/* over */ Set{1, 2}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add(Domain(/* over */ Set{1}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add(Domain(/* over */ Set{1, 2}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{});
  set.add(Domain(/* over */ Set{1, 3}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{}));

  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add(Domain(/* over */ Set{3}, /* under */ Set{}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 3}, /* under */ Set{}));

  // Test with under != over.
  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add(Domain(/* over */ Set{1, 2}, /* under */ Set{2}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add(Domain(/* over */ Set{1, 2}, /* under */ Set{2}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add(Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3}));
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2, 3}));
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, add_over) {
  auto set = Domain::bottom();
  set.add_over(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1}, /* under */ Set{}));

  set = Domain::bottom();
  set.add_over(Set{});
  EXPECT_EQ(set, Domain(/* over */ Set{}, /* under */ Set{}));

  set = Domain::top();
  set.add_over(1);
  EXPECT_EQ(set, Domain::top());

  set = Domain();
  set.add_over(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1}, /* under */ Set{}));

  // Test with over = under.
  set = Domain{1};
  set.add_over(1);
  EXPECT_EQ(set, Domain{1});

  set = Domain{1};
  set.add_over(2);
  EXPECT_EQ(set, (Domain(/* over */ Set{1, 2}, /* under */ Set{1})));

  set = Domain{1, 2};
  set.add_over(1);
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1, 2};
  set.add_over(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  // Test with under = empty.
  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add_over(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1}, /* under */ Set{}));

  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add_over(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add_over(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add_over(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add_over(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{}));

  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add_over(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 3}, /* under */ Set{}));

  // Test with under != over.
  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add_over(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add_over(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add_over(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add_over(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add_over(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add_over(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));
}

TEST_F(PatriciaTreeOverUnderSetAbstractDomainTest, add_under) {
  auto set = Domain::bottom();
  set.add_under(1);
  EXPECT_EQ(set, Domain(1));

  set = Domain::bottom();
  set.add_under(Set{});
  EXPECT_EQ(set, Domain());

  set = Domain::top();
  set.add_under(1);
  EXPECT_EQ(set, Domain::top());

  set = Domain();
  set.add_under(1);
  EXPECT_EQ(set, Domain(1));

  // Test with over = under.
  set = Domain{1};
  set.add_under(1);
  EXPECT_EQ(set, Domain{1});

  set = Domain{1};
  set.add_under(2);
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1, 2};
  set.add_under(1);
  EXPECT_EQ(set, (Domain{1, 2}));

  set = Domain{1, 2};
  set.add_under(3);
  EXPECT_EQ(set, (Domain{1, 2, 3}));

  // Test with under = empty.
  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add_under(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1}, /* under */ Set{1}));

  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add_under(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add_under(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{1}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add_under(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{});
  set.add_under(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{3}));

  set = Domain(/* over */ Set{1}, /* under */ Set{});
  set.add_under(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 3}, /* under */ Set{3}));

  // Test with under != over.
  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add_under(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add_under(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2}, /* under */ Set{2}));

  set = Domain(/* over */ Set{1, 2}, /* under */ Set{2});
  set.add_under(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{2, 3}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add_under(1);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add_under(2);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2}));

  set = Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2});
  set.add_under(3);
  EXPECT_EQ(set, Domain(/* over */ Set{1, 2, 3}, /* under */ Set{1, 2, 3}));
}
