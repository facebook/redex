/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gtest/gtest.h>

#include <sparta/AbstractDomain.h>

/*
 * Scaffold for testing that implementations of AbstractDomain satisfy various
 * properties.
 */

#define EXPECT_LEQ(D1, D2) \
  EXPECT_PRED2(([](auto d1, auto d2) { return d1.leq(d2); }), D1, D2)

#define EXPECT_NLEQ(D1, D2) \
  EXPECT_PRED2(([](auto d1, auto d2) { return !d1.leq(d2); }), D1, D2)

template <typename Domain>
class AbstractDomainPropertyTest : public ::testing::Test {
 public:
  /*
   * AbstractDomain implementors will probably want to specialize
   * non_extremal_values to get more test coverage. If there is more than one
   * possible internal state that corresponds to Top and Bottom, implementors
   * can also specialize top_values() and bottom_values() to check that all
   * states behave identically.
   */
  static std::vector<Domain> top_values() { return {Domain::top()}; }
  static std::vector<Domain> bottom_values() { return {Domain::bottom()}; }
  static std::vector<Domain> non_extremal_values() { return {}; }

  // We define these empty methods so that they can be template-specialized
  // by AbstractDomain implementors.
  static void SetUpTestCase() {}

  static void TearDownTestCase() {}

  static std::vector<Domain> all_values() {
    std::vector<Domain> result;
    const auto& tops = top_values();
    result.insert(result.end(), tops.begin(), tops.end());
    const auto& bottoms = bottom_values();
    result.insert(result.end(), bottoms.begin(), bottoms.end());
    const auto& others = non_extremal_values();
    result.insert(result.end(), others.begin(), others.end());
    return result;
  }
};

TYPED_TEST_CASE_P(AbstractDomainPropertyTest);

TYPED_TEST_P(AbstractDomainPropertyTest, Basics) {
  using Domain = TypeParam;

  for (const auto& dom : AbstractDomainPropertyTest<Domain>::top_values()) {
    EXPECT_TRUE(dom.is_top());
    EXPECT_FALSE(dom.is_bottom());
  }

  for (const auto& dom : AbstractDomainPropertyTest<Domain>::bottom_values()) {
    EXPECT_TRUE(dom.is_bottom());
    EXPECT_FALSE(dom.is_top());
  }

  for (auto dom : AbstractDomainPropertyTest<Domain>::non_extremal_values()) {
    EXPECT_FALSE(dom.is_top());
    dom.set_to_top();
    EXPECT_TRUE(dom.is_top());
  }

  for (auto dom : AbstractDomainPropertyTest<Domain>::non_extremal_values()) {
    EXPECT_FALSE(dom.is_bottom());
    dom.set_to_bottom();
    EXPECT_TRUE(dom.is_bottom());
  }
}

TYPED_TEST_P(AbstractDomainPropertyTest, JoinMeetBounds) {
  using Domain = TypeParam;

  for (const auto& d1 : AbstractDomainPropertyTest<Domain>::all_values()) {
    for (const auto& d2 : AbstractDomainPropertyTest<Domain>::all_values()) {
      EXPECT_LEQ(d1, d1.join(d2));
      EXPECT_LEQ(d2, d1.join(d2));
      EXPECT_LEQ(d1.meet(d2), d1);
      EXPECT_LEQ(d1.meet(d2), d2);
    }
  }
}

TYPED_TEST_P(AbstractDomainPropertyTest, Idempotence) {
  using Domain = TypeParam;

  for (const auto& dom : AbstractDomainPropertyTest<Domain>::all_values()) {
    EXPECT_EQ(dom.join(dom), dom);
    EXPECT_EQ(dom.meet(dom), dom);
  }
}

TYPED_TEST_P(AbstractDomainPropertyTest, Reflexivity) {
  using Domain = TypeParam;

  for (const auto& dom : AbstractDomainPropertyTest<Domain>::all_values()) {
    EXPECT_TRUE(dom.leq(dom));
    EXPECT_TRUE(dom.equals(dom));
  }
}

TYPED_TEST_P(AbstractDomainPropertyTest, Commutativity) {
  using Domain = TypeParam;

  for (const auto& d1 : AbstractDomainPropertyTest<Domain>::all_values()) {
    for (const auto& d2 : AbstractDomainPropertyTest<Domain>::all_values()) {
      if (d1.equals(d2)) {
        EXPECT_EQ(d1.leq(d2), d2.leq(d1))
            << "leq() not commutative on equal elements " << d1 << " and "
            << d2;
      } else {
        EXPECT_TRUE(!d1.leq(d2) || !d2.leq(d1));
      }
      EXPECT_EQ(d1.equals(d2), d2.equals(d1))
          << "equals() not commutative for " << d1 << " and " << d2;
      EXPECT_EQ(d1.join(d2), d2.join(d1))
          << "join() not commutative for " << d1 << " and " << d2;
      EXPECT_EQ(d1.meet(d2), d2.meet(d1))
          << "meet() not commutative for " << d1 << " and " << d2;
    }
  }
}

TYPED_TEST_P(AbstractDomainPropertyTest, Absorption) {
  using Domain = TypeParam;

  for (const auto& d1 : AbstractDomainPropertyTest<Domain>::all_values()) {
    for (const auto& d2 : AbstractDomainPropertyTest<Domain>::all_values()) {
      EXPECT_EQ(d1.join(d1.meet(d2)), d1);
      EXPECT_EQ(d1.meet(d1.join(d2)), d1);
    }
  }
}

TYPED_TEST_P(AbstractDomainPropertyTest, Relations) {
  using Domain = TypeParam;

  for (const auto& d1 : AbstractDomainPropertyTest<Domain>::top_values()) {
    for (const auto& d2 : AbstractDomainPropertyTest<Domain>::top_values()) {
      EXPECT_LEQ(d1, d2);
      EXPECT_EQ(d1, d2);

      EXPECT_TRUE(d1.join(d2).is_top());
      EXPECT_TRUE(d1.meet(d2).is_top());
    }
  }

  for (const auto& d1 : AbstractDomainPropertyTest<Domain>::bottom_values()) {
    for (const auto& d2 : AbstractDomainPropertyTest<Domain>::bottom_values()) {
      EXPECT_LEQ(d1, d2);
      EXPECT_EQ(d1, d2);

      EXPECT_TRUE(d1.join(d2).is_bottom());
      EXPECT_TRUE(d1.meet(d2).is_bottom());
    }
  }

  for (const auto& top : AbstractDomainPropertyTest<Domain>::top_values()) {
    for (const auto& bottom :
         AbstractDomainPropertyTest<Domain>::bottom_values()) {
      EXPECT_LEQ(bottom, top);
      EXPECT_NE(bottom, top);

      EXPECT_TRUE(top.join(bottom).is_top());
      EXPECT_TRUE(top.meet(bottom).is_bottom());
    }
  }

  for (const auto& top : AbstractDomainPropertyTest<Domain>::top_values()) {
    for (const auto& val :
         AbstractDomainPropertyTest<Domain>::non_extremal_values()) {
      EXPECT_LEQ(val, top);
      EXPECT_NE(val, top);

      EXPECT_TRUE(top.join(val).is_top());
      EXPECT_EQ(top.meet(val), val);
    }
  }

  for (const auto& bottom :
       AbstractDomainPropertyTest<Domain>::bottom_values()) {
    for (const auto& val :
         AbstractDomainPropertyTest<Domain>::non_extremal_values()) {
      EXPECT_LEQ(bottom, val);
      EXPECT_NE(val, bottom);

      EXPECT_TRUE(bottom.meet(val).is_bottom());
      EXPECT_EQ(bottom.join(val), val);
    }
  }
}

REGISTER_TYPED_TEST_CASE_P(AbstractDomainPropertyTest,
                           Basics,
                           JoinMeetBounds,
                           Idempotence,
                           Reflexivity,
                           Commutativity,
                           Absorption,
                           Relations);
