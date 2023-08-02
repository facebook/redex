/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Because of the rules of argument-dependent lookup, we need to include the
// definition of operator<< for ConstantAbstractDomain before that of operator<<
// for DisjointUnionAbstractDomain.
#include <sparta/ConstantAbstractDomain.h>

#include <sparta/DisjointUnionAbstractDomain.h>

#include <gtest/gtest.h>
#include <string>

#include "AbstractDomainPropertyTest.h"

using namespace sparta;

using IntDomain = ConstantAbstractDomain<int>;
using StringDomain = ConstantAbstractDomain<std::string>;
using IntStringDomain = DisjointUnionAbstractDomain<IntDomain, StringDomain>;

INSTANTIATE_TYPED_TEST_CASE_P(DisjointUnionAbstractDomain,
                              AbstractDomainPropertyTest,
                              IntStringDomain);

template <>
std::vector<IntStringDomain>
AbstractDomainPropertyTest<IntStringDomain>::top_values() {
  return {IntDomain::top(), StringDomain::top()};
}

template <>
std::vector<IntStringDomain>
AbstractDomainPropertyTest<IntStringDomain>::bottom_values() {
  return {IntDomain::bottom(), StringDomain::bottom()};
}

template <>
std::vector<IntStringDomain>
AbstractDomainPropertyTest<IntStringDomain>::non_extremal_values() {
  return {IntDomain(0), StringDomain("foo")};
}

TEST(DisjointUnionAbstractDomainTest, basicOperations) {
  IntStringDomain zero = IntDomain(0);
  IntStringDomain str = StringDomain("");
  EXPECT_TRUE(zero.join(str).is_top());
  EXPECT_TRUE(zero.meet(str).is_bottom());
  EXPECT_NLEQ(zero, str);
  EXPECT_NLEQ(str, zero);
  EXPECT_NE(zero, str);
}
