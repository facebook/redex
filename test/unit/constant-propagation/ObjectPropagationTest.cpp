/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationTestUtil.h"

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

TEST_F(ConstantPropagationTest, ConstantEnvironmentObjectOperations) {
  ConstantEnvironment env;

  auto insn = std::make_unique<IRInstruction>(OPCODE_NEW_INSTANCE);
  insn->set_type(DexType::make_type("LFoo;"));
  env.new_heap_value(1, insn.get(), ConstantObjectDomain());
  EXPECT_EQ(env.get<AbstractHeapPointer>(1), AbstractHeapPointer(insn.get()));

  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.bar:I"));
  env.set_object_field(1, field, SignedConstantDomain(1));
  EXPECT_EQ(
      env.get_pointee<ConstantObjectDomain>(1).get<SignedConstantDomain>(field),
      SignedConstantDomain(1));
}
