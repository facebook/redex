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

  ConstantValue sod{SingletonObjectDomain(/* field */ nullptr)};

  ConstantValue owia{
      ObjectWithImmutAttrDomain(ObjectWithImmutAttr(/* type */ nullptr, 0))};
};

INSTANTIATE_TYPED_TEST_CASE_P(ConstantValue,
                              AbstractDomainPropertyTest,
                              ConstantValue);

template <>
std::vector<ConstantValue>
AbstractDomainPropertyTest<ConstantValue>::non_extremal_values() {
  Constants constants;
  return {constants.one, constants.zero, constants.nez, constants.sod,
          constants.owia};
}

class ConstantValueTest : public testing::Test, public Constants {};

TEST_F(ConstantValueTest, meet) {
  using namespace sign_domain;

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
}
