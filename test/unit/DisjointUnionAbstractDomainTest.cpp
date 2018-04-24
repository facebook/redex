/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DisjointUnionAbstractDomain.h"

#include <gtest/gtest.h>
#include <string>

#include "ConstantAbstractDomain.h"

using IntDomain = ConstantAbstractDomain<int>;
using StringDomain = ConstantAbstractDomain<std::string>;
using IntStringDomain = DisjointUnionAbstractDomain<IntDomain, StringDomain>;

TEST(DisjointUnionAbstractDomainTest, basicOperations) {
  IntStringDomain zero = IntDomain(0);
  IntStringDomain str = StringDomain("");
  EXPECT_EQ(zero.join(zero), zero);
  EXPECT_EQ(str.meet(str), str);
  EXPECT_TRUE(zero.join(str).is_top());
  EXPECT_EQ(zero.join(StringDomain::bottom()), zero);
  EXPECT_TRUE(zero.meet(str).is_bottom());
  EXPECT_EQ(str.meet(IntDomain::top()), str);
  EXPECT_FALSE(zero.leq(str));
  EXPECT_FALSE(str.leq(zero));
  EXPECT_TRUE(zero.leq(StringDomain::top()));
  EXPECT_FALSE(IntStringDomain(StringDomain::top()).leq(zero));
  EXPECT_FALSE(zero.leq(StringDomain::bottom()));
  EXPECT_TRUE(IntStringDomain(StringDomain::bottom()).leq(zero));
  EXPECT_NE(zero, str);

  // Check that we have the same value for Top / Bottom regardless of which
  // variant we used to construct it.
  EXPECT_EQ(IntStringDomain(IntDomain::top()),
            IntStringDomain(StringDomain::top()));
  EXPECT_EQ(IntStringDomain(IntDomain::bottom()),
            IntStringDomain(StringDomain::bottom()));
}
