/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "SignedConstantDomain.h"

struct Constants {
  ConstantValue one{SignedConstantDomain(1)};

  ConstantValue zero{SignedConstantDomain(0)};

  ConstantValue nez{SignedConstantDomain::nez()};

  ConstantValue sod{SingletonObjectDomain(
      (DexField*)DexField::make_field("LFoo;.bar:LFoo;"))};

  ConstantValue owia{ObjectWithImmutAttrDomain(
      ObjectWithImmutAttr(DexType::make_type("LFoo;"), 0))};

  ConstantValue sd_a{StringDomain(DexString::make_string("A"))};

  ConstantValue sd_b{StringDomain(DexString::make_string("B"))};
};

INSTANTIATE_TYPED_TEST_CASE_P(ConstantValue,
                              AbstractDomainPropertyTest,
                              ConstantValue);

template <>
void AbstractDomainPropertyTest<ConstantValue>::SetUpTestCase() {
  g_redex = new RedexContext();
}

template <>
void AbstractDomainPropertyTest<ConstantValue>::TearDownTestCase() {
  delete g_redex;
}

template <>
std::vector<ConstantValue>
AbstractDomainPropertyTest<ConstantValue>::non_extremal_values() {
  Constants constants;
  return {
      constants.one, constants.zero, constants.nez,
      constants.sod, constants.sd_a, constants.sd_b,
      // constants.owia FIXME. The meet of ObjectWithImmutAttrDomain with
      // itself, and with SingletonObjectDomain, can go to top(), which is
      // wrong.
  };
}

class ConstantValueTest : public RedexTest, public Constants {};

TEST_F(ConstantValueTest, meet) {
  using namespace sign_domain;

  auto meet = [](const auto& x, const auto& y) { return x.meet(y); };

  EXPECT_EQ(meet(zero, sod), ConstantValue::bottom());
  EXPECT_EQ(meet(nez, sod), sod);
  EXPECT_EQ(meet(sod, nez), sod);
  EXPECT_EQ(meet(ConstantValue::top(), sod), sod);
  EXPECT_EQ(meet(sod, ConstantValue::top()), sod);

  EXPECT_EQ(meet(zero, owia), ConstantValue::bottom());
  EXPECT_EQ(meet(nez, owia), owia);
  EXPECT_EQ(meet(owia, nez), owia);
  EXPECT_EQ(meet(ConstantValue::top(), owia), owia);
  EXPECT_EQ(meet(owia, ConstantValue::top()), owia);

  EXPECT_EQ(meet(sod, owia), ConstantValue::top());
  EXPECT_EQ(meet(owia, sod), ConstantValue::top());

  EXPECT_EQ(meet(sd_a, sd_b), ConstantValue::bottom());
  EXPECT_EQ(meet(sd_b, sd_a), ConstantValue::bottom());
}

TEST_F(ConstantValueTest, join) {
  using namespace sign_domain;

  auto join = [](const auto& x, const auto& y) { return x.join(y); };

  EXPECT_EQ(join(zero, sod), ConstantValue::top());
  EXPECT_EQ(join(nez, sod), nez);
  EXPECT_EQ(join(sod, nez), nez);
  EXPECT_EQ(join(ConstantValue::top(), sod), ConstantValue::top());
  EXPECT_EQ(join(sod, ConstantValue::top()), ConstantValue::top());

  EXPECT_EQ(join(zero, owia), ConstantValue::top());
  EXPECT_EQ(join(nez, owia), nez);
  EXPECT_EQ(join(owia, nez), nez);
  EXPECT_EQ(join(ConstantValue::top(), owia), ConstantValue::top());
  EXPECT_EQ(join(owia, ConstantValue::top()), ConstantValue::top());

  EXPECT_EQ(join(sod, owia), ConstantValue::top());
  EXPECT_EQ(join(owia, sod), ConstantValue::top());

  EXPECT_EQ(join(sd_a, sd_b), nez);
  EXPECT_EQ(join(sd_b, sd_a), nez);
}
