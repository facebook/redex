/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagation.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationTestUtil.h"
#include "ObjectDomain.h"

using ConstantObjectDomain = ObjectDomain<ConstantValue>;

INSTANTIATE_TYPED_TEST_CASE_P(ConstantObjectDomain,
                              AbstractDomainPropertyTest,
                              ConstantObjectDomain);

// We need RedexContext to be set up in order to call DexField::make_field...
// XXX find a way to initialize this in a less hacky fashion.
template <>
void AbstractDomainPropertyTest<ConstantObjectDomain>::SetUpTestCase() {
  g_redex = new RedexContext();
}

template <>
void AbstractDomainPropertyTest<ConstantObjectDomain>::TearDownTestCase() {
  delete g_redex;
}

template <>
std::vector<ConstantObjectDomain>
AbstractDomainPropertyTest<ConstantObjectDomain>::non_extremal_values() {
  ConstantObjectDomain empty_unescaped;
  ConstantObjectDomain one_field;
  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.bar:I"));
  field->make_concrete(ACC_PUBLIC);
  one_field.set(field, SignedConstantDomain(1));
  return {empty_unescaped, one_field};
}

TEST_F(ConstantPropagationTest, ObjectOperations) {
  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.bar:I"));

  ConstantObjectDomain obj;
  EXPECT_FALSE(obj.is_escaped());
  // Note that the default-constructed value is not Top.
  EXPECT_FALSE(obj.is_top());

  // Check that writing / reading from a non-escaped object works.
  obj.set(field, SignedConstantDomain(1));
  EXPECT_EQ(obj.get(field), SignedConstantDomain(1));
  obj.set_escaped();
  EXPECT_TRUE(obj.is_escaped());
  EXPECT_TRUE(obj.is_top());

  // Check that writing to an escaped object is a no-op.
  EXPECT_EQ(obj.get(field), ConstantValue::top());
  obj.set(field, SignedConstantDomain(1));
  EXPECT_EQ(obj.get(field), ConstantValue::top());
  EXPECT_TRUE(obj.is_top());
}
