/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Resolver.h"
#include "verify/VerifyUtil.h"

// Check builder is actually defined.
TEST_F(PreVerify, RemoveTestBuilder) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_NE(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
}

// Ensure the builder was removed.
TEST_F(PostVerify, RemoveTestBuilder) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_EQ(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_removed =
      find_vmethod_named(*test_builder, "testRemoveBuilder");
  EXPECT_NE(nullptr, test_builder_removed);
}

// Check builder is actually defined.
TEST_F(PreVerify, RemoveTestBuilderWithStaticField) {
  auto builder_cls = find_class_named(
      classes,
      "Lcom/facebook/redex/test/instr/LithoComponentWithStaticFields$Builder;");
  EXPECT_NE(nullptr, builder_cls);
}

// Ensure the builder was removed.
TEST_F(PostVerify, RemoveTestBuilderWithStaticField) {
  auto builder_cls = find_class_named(
      classes,
      "Lcom/facebook/redex/test/instr/LithoComponentWithStaticFields$Builder;");
  EXPECT_NE(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_removed = find_vmethod_named(
      *test_builder, "testRemoveBuilderAllocationWithStaticFields");
  EXPECT_NE(nullptr, test_builder_removed);
  test_builder_removed->balloon();

  auto code = test_builder_removed->get_code();
  auto builder = builder_cls->get_type();
  auto super_type = builder_cls->get_super_class();

  size_t num_builder_static_accesses = 0;

  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_method()) {
      auto method = resolve_method(
          insn->get_method(), opcode_to_search(insn), test_builder_removed);
      if (!method) {
        continue;
      }

      EXPECT_TRUE(method->get_class() != builder);
      if (method->get_class() == super_type) {
        EXPECT_TRUE(is_static(method));
      }
    } else if (insn->has_type()) {
      EXPECT_FALSE(insn->get_type() == builder ||
                   insn->get_type() == super_type);
    } else if (insn->has_field()) {
      auto field = resolve_field(insn->get_field(), FieldSearch::Any);
      if (field->get_class() == builder || field->get_class() == super_type) {
        EXPECT_TRUE(is_static(field));
        num_builder_static_accesses++;
      }
    }
  }

  EXPECT_EQ(1, num_builder_static_accesses);
}

TEST_F(PreVerify, RemoveTestBuilderWhenCheckIfNull) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_NE(nullptr, builder_cls);
}

TEST_F(PostVerify, RemoveTestBuilderWhenCheckIfNull) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_EQ(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);

  auto test_builder_removed =
      find_vmethod_named(*test_builder, "testWhenCheckIfNull");
  auto code = test_builder_removed->get_code();
}

TEST_F(PostVerify, TestRemoveIfConditionallyCreated) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_EQ(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);

  auto test_builder_removed =
      find_vmethod_named(*test_builder, "testRemoveIfConditionallyCreated");
  auto code = test_builder_removed->get_code();
}

namespace {

bool type_accessed(DexMethod* method, DexType* type) {
  for (const auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (insn->has_type() && insn->get_type() == type) {
      return true;
    } else if (insn->has_method()) {
      if (insn->get_method()->get_class() == type) {
        return true;
      }
      auto rtype = insn->get_method()->get_proto()->get_rtype();
      if (rtype == type) {
        return true;
      }
    } else if (insn->has_field()) {
      if (insn->get_field()->get_class() == type ||
          insn->get_field()->get_type() == type) {
        return true;
      }
    }
  }

  return false;
}

} // namespace

TEST_F(PreVerify, DontRemoveWhenDifferentInstancesCreated) {
  auto builder_A = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentA$Builder;");
  EXPECT_NE(nullptr, builder_A);

  auto builder_B = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentB$Builder;");
  EXPECT_NE(nullptr, builder_B);
}

TEST_F(PostVerify, DontRemoveWhenDifferentInstancesCreated) {
  auto builder_A = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentA$Builder;");
  EXPECT_NE(nullptr, builder_A);

  auto builder_B = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentB$Builder;");
  EXPECT_NE(nullptr, builder_B);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto non_removed = find_vmethod_named(
      *test_builder, "nonRemovedIfDifferentInstancesCreated");
  EXPECT_NE(nullptr, non_removed);
  non_removed->balloon();

  EXPECT_TRUE(type_accessed(non_removed, builder_A->get_type()));
  EXPECT_TRUE(type_accessed(non_removed, builder_B->get_type()));
}

TEST_F(PreVerify, DontRemoveIfStored) {
  auto builder_A = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentA$Builder;");
  EXPECT_NE(nullptr, builder_A);
}

TEST_F(PostVerify, DontRemoveIfStored) {
  auto builder_A = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentA$Builder;");
  EXPECT_NE(nullptr, builder_A);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto non_removed = find_vmethod_named(*test_builder, "nonRemovedIfStored");
  EXPECT_NE(nullptr, non_removed);
  non_removed->balloon();

  EXPECT_TRUE(type_accessed(non_removed, builder_A->get_type()));
}

TEST_F(PreVerify, RemoveIfUsedInAConditionalCheck) {
  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_method =
      find_vmethod_named(*test_builder, "removeIfUsedInAConditionBranch");
  EXPECT_NE(nullptr, test_builder_method);
  test_builder_method->balloon();

  auto builder = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_NE(nullptr, builder);
  EXPECT_TRUE(type_accessed(test_builder_method, builder->get_type()));
}

TEST_F(PostVerify, RemoveIfUsedInAConditionalCheck) {
  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_method =
      find_vmethod_named(*test_builder, "removeIfUsedInAConditionBranch");
  EXPECT_NE(nullptr, test_builder_method);

  auto builder = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_EQ(nullptr, builder);
}

TEST_F(PreVerify, DontRemoveIfReturned) {
  auto builder_B = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentB$Builder;");
  EXPECT_NE(nullptr, builder_B);
}

TEST_F(PostVerify, DontRemoveIfReturned) {
  auto builder_B = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentB$Builder;");
  EXPECT_NE(nullptr, builder_B);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  auto non_removed = find_vmethod_named(*test_builder, "nonRemovedIfReturned");
  EXPECT_NE(nullptr, non_removed);
  non_removed->balloon();

  EXPECT_TRUE(type_accessed(non_removed, builder_B->get_type()));
}

namespace {

std::unordered_set<int64_t> get_const_literals(DexMethod* method) {
  std::unordered_set<int64_t> res;

  for (const auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (opcode::is_a_const(insn->opcode()) && insn->has_literal()) {
      res.emplace(insn->get_literal());
    }
  }

  return res;
}

} // namespace

// Check builder is actually defined.
TEST_F(PreVerify, RemoveTestBuilderUsedInANotNullCheck) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_NE(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);

  auto test_builder_removed =
      find_vmethod_named(*test_builder, "removeBuilderForNotNullCheck");
  EXPECT_NE(nullptr, test_builder_removed);
  test_builder_removed->balloon();

  // Check that we either pass 7 or 8 as a prop value to the builder.
  auto const_literals = get_const_literals(test_builder_removed);
  EXPECT_TRUE(const_literals.count(7));
  EXPECT_TRUE(const_literals.count(8));
}

// Ensure the builder was removed.
TEST_F(PostVerify, RemoveTestBuilderUsedInANotNullCheck) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_EQ(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_removed =
      find_vmethod_named(*test_builder, "removeBuilderForNotNullCheck");
  EXPECT_NE(nullptr, test_builder_removed);
  test_builder_removed->balloon();

  // Check that we only pass 8 as a prop value to the builder.
  auto const_literals = get_const_literals(test_builder_removed);
  EXPECT_TRUE(const_literals.count(8) > 0);
  EXPECT_TRUE(const_literals.count(7) == 0);
}

// Check builder is actually defined.
TEST_F(PreVerify, RemoveTestBuilderUsedInANullCheck) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_NE(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);

  auto test_builder_removed =
      find_vmethod_named(*test_builder, "removeBuilderForNullCheck");
  EXPECT_NE(nullptr, test_builder_removed);
  test_builder_removed->balloon();

  // Check that we eiher pass 7 or 8 as a prop value to the builder.
  auto const_literals = get_const_literals(test_builder_removed);
  EXPECT_TRUE(const_literals.count(7));
  EXPECT_TRUE(const_literals.count(8));
}

// Ensure the builder was removed.
TEST_F(PostVerify, RemoveTestBuilderUsedInANullCheck) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/LithoComponent$Builder;");
  EXPECT_EQ(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_removed =
      find_vmethod_named(*test_builder, "removeBuilderForNullCheck");
  EXPECT_NE(nullptr, test_builder_removed);
  test_builder_removed->balloon();

  // Check that we only pass 7 as a prop value to the builder.
  auto const_literals = get_const_literals(test_builder_removed);
  EXPECT_TRUE(const_literals.count(8) == 0);
  EXPECT_TRUE(const_literals.count(7) > 0);
}

// Ensure the builder was not removed.
TEST_F(PostVerify, DontRemoveBuilderIfUsedForSynchronization) {
  auto builder_cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentC$Builder;");
  EXPECT_NE(nullptr, builder_cls);

  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_method =
      find_vmethod_named(*test_builder, "notRemovedIfUsedForSynchronization");
  EXPECT_NE(nullptr, test_builder_method);

  test_builder_method->balloon();
  EXPECT_TRUE(type_accessed(test_builder_method, builder_cls->get_type()));
}

TEST_F(PreVerify, NonRemovedIfInstanceOfUsed) {
  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_method =
      find_vmethod_named(*test_builder, "nonRemovedIfInstanceOfUsed");
  EXPECT_NE(nullptr, test_builder_method);
  test_builder_method->balloon();

  auto builder = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentD$Builder;");
  EXPECT_NE(nullptr, builder);
  EXPECT_TRUE(type_accessed(test_builder_method, builder->get_type()));
}

TEST_F(PostVerify, NonRemovedIfInstanceOfUsed) {
  auto test_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/TestBuilder;");
  EXPECT_NE(nullptr, test_builder);
  auto test_builder_method =
      find_vmethod_named(*test_builder, "nonRemovedIfInstanceOfUsed");
  EXPECT_NE(nullptr, test_builder_method);
  test_builder_method->balloon();

  auto builder = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/TestingComponentD$Builder;");
  EXPECT_NE(nullptr, builder);
  EXPECT_TRUE(type_accessed(test_builder_method, builder->get_type()));
}

TEST_F(PreVerify, SimpleBuilder) {
  auto builder = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/Model$Builder;");
  EXPECT_NE(nullptr, builder);
}

TEST_F(PostVerify, SimpleBuilder) {
  auto builder = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/Model$Builder;");
  EXPECT_EQ(nullptr, builder);
}
