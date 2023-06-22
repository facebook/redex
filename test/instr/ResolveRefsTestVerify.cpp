/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "DexInstruction.h"
#include "VerifyUtil.h"

TEST_F(PreVerify, SimpleInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testSimpleInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}

TEST_F(PostVerify, SimpleInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testSimpleInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}

TEST_F(PostVerify, FactoryBaseInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testFactoryBaseInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));

  // rtype is specialized
  auto s1_getinstance = find_dmethod_named(*sub1_cls, "getInstance");
  ASSERT_NE(nullptr, s1_getinstance);
  ASSERT_EQ(s1_getinstance->get_proto()->get_rtype(), sub1_cls->get_type());
}

TEST_F(PostVerify, FactoryCastInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testFactoryCastInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}

TEST_F(PreVerify, SimpleRTypeSpecialization) {
  auto intf_cls = find_class_named(classes, "Lcom/facebook/redextest/Intf;");
  ASSERT_NE(nullptr, intf_cls);
  auto impl_cls = find_class_named(classes, "Lcom/facebook/redextest/Impl;");
  ASSERT_NE(nullptr, impl_cls);

  // rtype is specialized
  auto intf_getinstance = find_vmethod_named(*intf_cls, "getInstance");
  ASSERT_NE(nullptr, intf_getinstance);
  ASSERT_EQ(intf_getinstance->get_proto()->get_rtype(), intf_cls->get_type());

  auto impl_getinstance = find_vmethod_named(*impl_cls, "getInstance");
  ASSERT_NE(nullptr, impl_getinstance);
  ASSERT_EQ(impl_getinstance->get_proto()->get_rtype(), intf_cls->get_type());
}

TEST_F(PostVerify, SimpleRTypeSpecialization) {
  auto intf_cls = find_class_named(classes, "Lcom/facebook/redextest/Intf;");
  ASSERT_NE(nullptr, intf_cls);
  auto impl_cls = find_class_named(classes, "Lcom/facebook/redextest/Impl;");
  ASSERT_NE(nullptr, impl_cls);

  // rtype is specialized
  auto intf_getinstance = find_vmethod_named(*intf_cls, "getInstance");
  ASSERT_NE(nullptr, intf_getinstance);
  ASSERT_EQ(intf_getinstance->get_proto()->get_rtype(), impl_cls->get_type());

  auto impl_getinstance = find_vmethod_named(*impl_cls, "getInstance");
  ASSERT_NE(nullptr, impl_getinstance);
  ASSERT_EQ(impl_getinstance->get_proto()->get_rtype(), impl_cls->get_type());
}

TEST_F(PostVerify, RTypeSpecializationCollision) {
  auto ani_cls = find_class_named(classes, "Lcom/facebook/redextest/Animal;");
  ASSERT_NE(nullptr, ani_cls);
  auto cat_cls = find_class_named(classes, "Lcom/facebook/redextest/Cat;");
  ASSERT_NE(nullptr, cat_cls);
  auto badcat_cls =
      find_class_named(classes, "Lcom/facebook/redextest/NotYourFavoriteCat;");
  ASSERT_NE(nullptr, badcat_cls);

  // rtype is not specializd due to collision.
  auto cat_foo = find_vmethod_named(*cat_cls, "foo");
  ASSERT_NE(nullptr, cat_foo);
  ASSERT_EQ(cat_foo->get_proto()->get_rtype(), ani_cls->get_type());
  // Likewise, rtype is not specialized in the subclass.
  auto* foo_ani_proto = DexProto::make_proto(ani_cls->get_type(),
                                             cat_foo->get_proto()->get_args());
  auto* foo_ani = find_vmethod(*badcat_cls, "foo", foo_ani_proto);
  ASSERT_NE(nullptr, foo_ani);

  // However, NotYourFavoriteCat;.foo()Cat; is specialized to
  // NotYourFavoriteCat;.foo()NotYourFavoriteCat;
  auto* foo_badcat_proto = DexProto::make_proto(
      badcat_cls->get_type(), cat_foo->get_proto()->get_args());
  ASSERT_NE(nullptr, foo_badcat_proto);
  auto foo_badcat = find_vmethod(*badcat_cls, "foo", foo_badcat_proto);
  ASSERT_NE(nullptr, foo_badcat);
  auto* foo_cat_proto = DexProto::make_proto(cat_cls->get_type(),
                                             cat_foo->get_proto()->get_args());
  auto* foo_cat = find_vmethod(*badcat_cls, "foo", foo_cat_proto);
  ASSERT_EQ(nullptr, foo_cat);
}

TEST_F(PreVerify, ResolveMirandaToInterface) {
  auto conc_cls = find_class_named(classes, "Lcom/facebook/redextest/Concept;");
  ASSERT_NE(nullptr, conc_cls);
  auto incomp_cls =
      find_class_named(classes, "Lcom/facebook/redextest/Incomplete;");
  ASSERT_NE(nullptr, incomp_cls);
  auto comp_cls =
      find_class_named(classes, "Lcom/facebook/redextest/Complete;");
  ASSERT_NE(nullptr, comp_cls);

  // Miranda pure ref
  auto incomp_getfake = find_vmethod_named(*incomp_cls, "getFake");
  ASSERT_NE(nullptr, incomp_getfake);
  ASSERT_NE(nullptr, find_invoke(incomp_getfake, DOPCODE_INVOKE_VIRTUAL,
                                 "getReal", incomp_cls->get_type()));

  // Interface virtual scope rtype not specialized
  auto conc_getreal = find_vmethod_named(*conc_cls, "getReal");
  ASSERT_EQ(conc_getreal->get_proto()->get_rtype(), conc_cls->get_type());
  auto comp_getreal = find_vmethod_named(*comp_cls, "getReal");
  ASSERT_EQ(comp_getreal->get_proto()->get_rtype(), conc_cls->get_type());
}

TEST_F(PostVerify, ResolveMirandaToInterface) {
  auto conc_cls = find_class_named(classes, "Lcom/facebook/redextest/Concept;");
  ASSERT_NE(nullptr, conc_cls);
  auto incomp_cls =
      find_class_named(classes, "Lcom/facebook/redextest/Incomplete;");
  ASSERT_NE(nullptr, incomp_cls);
  auto comp_cls =
      find_class_named(classes, "Lcom/facebook/redextest/Complete;");
  ASSERT_NE(nullptr, comp_cls);

  // Pure ref resolved to interface method
  auto incomp_getfake = find_vmethod_named(*incomp_cls, "getFake");
  ASSERT_NE(nullptr, incomp_getfake);
  ASSERT_NE(nullptr, find_invoke(incomp_getfake, DOPCODE_INVOKE_INTERFACE,
                                 "getReal", conc_cls->get_type()));

  // Interface virtual scope rtype specialized
  auto conc_getreal = find_vmethod_named(*conc_cls, "getReal");
  ASSERT_EQ(conc_getreal->get_proto()->get_rtype(), comp_cls->get_type());
  auto comp_getreal = find_vmethod_named(*comp_cls, "getReal");
  ASSERT_EQ(comp_getreal->get_proto()->get_rtype(), comp_cls->get_type());
}
