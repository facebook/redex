/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/PatriciaTreeSet.h>
#include <sparta/PatriciaTreeSetAbstractDomain.h>

#include <algorithm>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace sparta;

using Domain = PatriciaTreeSetAbstractDomain<std::string*>;

class PatriciaTreeSetAbstractDomainTest : public ::testing::Test {
 protected:
  PatriciaTreeSetAbstractDomainTest()
      : m_a(new std::string("a")),
        m_b(new std::string("b")),
        m_c(new std::string("c")),
        m_d(new std::string("d")) {}

  std::string* a() { return m_a.get(); }
  std::string* b() { return m_b.get(); }
  std::string* c() { return m_c.get(); }
  std::string* d() { return m_d.get(); }
  std::string* e() { return m_e.get(); }

  std::vector<std::string> to_string(const PatriciaTreeSet<std::string*>& s) {
    std::vector<std::string> result;
    std::transform(s.begin(),
                   s.end(),
                   std::back_inserter(result),
                   [](std::string* p) { return *p; });
    return result;
  }

  std::unique_ptr<std::string> m_a;
  std::unique_ptr<std::string> m_b;
  std::unique_ptr<std::string> m_c;
  std::unique_ptr<std::string> m_d;
  std::unique_ptr<std::string> m_e;
};

TEST_F(PatriciaTreeSetAbstractDomainTest, latticeOperations) {
  Domain e1(this->a());
  Domain e2({this->a(), this->b(), this->c()});
  Domain e3({this->b(), this->c(), this->d()});

  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a"));
  EXPECT_THAT(this->to_string(e2.elements()),
              ::testing::UnorderedElementsAre("a", "b", "c"));
  EXPECT_THAT(this->to_string(e3.elements()),
              ::testing::UnorderedElementsAre("b", "c", "d"));

  EXPECT_TRUE(Domain::bottom().leq(Domain::top()));
  EXPECT_FALSE(Domain::top().leq(Domain::bottom()));
  EXPECT_FALSE(e2.is_top());
  EXPECT_FALSE(e2.is_bottom());

  EXPECT_TRUE(e1.leq(e2));
  EXPECT_FALSE(e1.leq(e3));
  EXPECT_TRUE(e2.equals(Domain({this->b(), this->c(), this->a()})));
  EXPECT_FALSE(e2.equals(e3));

  EXPECT_THAT(this->to_string(e2.join(e3).elements()),
              ::testing::UnorderedElementsAre("a", "b", "c", "d"));
  EXPECT_TRUE(e1.join(e2).equals(e2));
  EXPECT_TRUE(e2.join(Domain::bottom()).equals(e2));
  EXPECT_TRUE(e2.join(Domain::top()).is_top());
  EXPECT_TRUE(e1.widening(e2).equals(e2));

  EXPECT_THAT(this->to_string(e2.meet(e3).elements()),
              ::testing::UnorderedElementsAre("b", "c"));
  EXPECT_TRUE(e1.meet(e2).equals(e1));
  EXPECT_TRUE(e2.meet(Domain::bottom()).is_bottom());
  EXPECT_TRUE(e2.meet(Domain::top()).equals(e2));
  EXPECT_FALSE(e1.meet(e3).is_bottom());
  EXPECT_TRUE(e1.meet(e3).elements().empty());
  EXPECT_TRUE(e1.narrowing(e2).equals(e1));

  EXPECT_TRUE(e2.contains(this->a()));
  EXPECT_FALSE(e3.contains(this->a()));

  // Making sure no side effect took place.
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a"));
  EXPECT_THAT(this->to_string(e2.elements()),
              ::testing::UnorderedElementsAre("a", "b", "c"));
  EXPECT_THAT(this->to_string(e3.elements()),
              ::testing::UnorderedElementsAre("b", "c", "d"));
}

TEST_F(PatriciaTreeSetAbstractDomainTest, destructiveOperations) {
  Domain e1(this->a());
  Domain e2({this->a(), this->b(), this->c()});
  Domain e3({this->b(), this->c(), this->d()});

  e1.add(this->b());
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a", "b"));
  e1.add({this->a(), this->c()});
  EXPECT_TRUE(e1.equals(e2));
  std::vector<std::string*> v1 = {this->a(), this->b()};
  e1.add(v1.begin(), v1.end());
  EXPECT_TRUE(e1.equals(e2));

  e1.remove(this->b());
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a", "c"));
  e1.remove(this->d());
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a", "c"));
  std::vector<std::string*> v2 = {this->a(), this->e()};
  e1.remove(v2.begin(), v2.end());
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("c"));
  e1.remove({this->a(), this->c()});
  EXPECT_TRUE(e1.elements().empty());

  e1.join_with(e2);
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a", "b", "c"));
  e1.join_with(Domain::bottom());
  EXPECT_TRUE(e1.equals(e2));
  e1.join_with(Domain::top());
  EXPECT_TRUE(e1.is_top());

  e1 = Domain(this->a());
  e1.widen_with(Domain({this->b(), this->c()}));
  EXPECT_TRUE(e1.equals(e2));

  e1 = Domain(this->a());
  e2.meet_with(e3);
  EXPECT_THAT(this->to_string(e2.elements()),
              ::testing::UnorderedElementsAre("b", "c"));
  e1.meet_with(e2);
  EXPECT_TRUE(e1.elements().empty());
  e1.meet_with(Domain::top());
  EXPECT_THAT(this->to_string(e2.elements()),
              ::testing::UnorderedElementsAre("b", "c"));
  e1.meet_with(Domain::bottom());
  EXPECT_TRUE(e1.is_bottom());

  e1 = Domain(this->a());
  e1.narrow_with(Domain({this->a(), this->b()}));
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a"));

  EXPECT_FALSE(e2.is_top());
  e1.set_to_top();
  EXPECT_TRUE(e1.is_top());
  e1.set_to_bottom();
  EXPECT_TRUE(e1.is_bottom());
  EXPECT_FALSE(e2.is_bottom());
  e2.set_to_bottom();
  EXPECT_TRUE(e2.is_bottom());

  e1 = Domain({this->a(), this->b(), this->c(), this->d()});
  e2 = e1;
  EXPECT_TRUE(e1.equals(e2));
  EXPECT_TRUE(e2.equals(e1));
  EXPECT_FALSE(e2.is_bottom());
  EXPECT_THAT(this->to_string(e2.elements()),
              ::testing::UnorderedElementsAre("a", "b", "c", "d"));

  e1 = Domain::top();
  e1.difference_with(Domain::bottom());
  EXPECT_TRUE(e1.is_top());
  e1.difference_with(Domain(this->a()));
  EXPECT_TRUE(e1.is_top());
  e1.difference_with(Domain::top());
  EXPECT_TRUE(e1.is_bottom());

  e1 = Domain::bottom();
  e1.difference_with(Domain::bottom());
  EXPECT_TRUE(e1.is_bottom());
  e1.difference_with(Domain(this->a()));
  EXPECT_TRUE(e1.is_bottom());
  e1.difference_with(Domain::top());
  EXPECT_TRUE(e1.is_bottom());

  e1 = Domain({this->a(), this->b(), this->c()});
  e1.difference_with(Domain::bottom());
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a", "b", "c"));
  e1.difference_with(Domain({this->b(), this->d()}));
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a", "c"));
  e1.difference_with(Domain({this->c()}));
  EXPECT_THAT(this->to_string(e1.elements()),
              ::testing::UnorderedElementsAre("a"));
  e1.difference_with(Domain::top());
  EXPECT_TRUE(e1.is_bottom());
}
