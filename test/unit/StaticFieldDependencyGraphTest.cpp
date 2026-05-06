/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "StaticFieldDependencyGraph.h"

using namespace clinit_batching;

class StaticFieldDependencyGraphTest : public RedexTest {
 protected:
  /**
   * Helper to add a dependency to the graph. Wraps the private
   * add_dependency method via the friend declaration.
   */
  static void add_dep(StaticFieldDependencyGraph& graph,
                      DexClass* from,
                      DexClass* to) {
    graph.add_dependency(from, to);
  }

  /**
   * Helper to create a simple class (for graph-only tests that don't need
   * clinit).
   */
  DexClass* create_class(const std::string& class_name) {
    auto* type = DexType::make_type(class_name);
    ClassCreator creator(type);
    creator.set_super(type::java_lang_Object());
    return creator.create();
  }

  /**
   * Helper to create a class with a static field and clinit.
   */
  DexClass* make_class_with_clinit(const std::string& class_name,
                                   const std::string& clinit_code) {
    auto* type = DexType::make_type(class_name);
    ClassCreator creator(type);
    creator.set_super(type::java_lang_Object());

    // Add a static field
    auto* field = DexField::make_field(class_name + ".value:I")
                      ->make_concrete(ACC_STATIC | ACC_PUBLIC);
    creator.add_field(field);

    // Create clinit method from assembler string
    auto* clinit = assembler::method_from_string(clinit_code);
    creator.add_method(clinit);

    return creator.create();
  }

  /**
   * Helper to verify that class A comes before class B in the sorted order.
   */
  bool comes_before(const std::vector<DexClass*>& ordered,
                    DexClass* a,
                    DexClass* b) {
    auto it_a = std::find(ordered.begin(), ordered.end(), a);
    auto it_b = std::find(ordered.begin(), ordered.end(), b);
    return it_a != ordered.end() && it_b != ordered.end() && it_a < it_b;
  }
};

TEST_F(StaticFieldDependencyGraphTest, test_empty_graph) {
  StaticFieldDependencyGraph graph;

  EXPECT_EQ(graph.size(), 0);

  auto result = graph.topological_sort();
  EXPECT_TRUE(result.ordered_classes.empty());
  EXPECT_TRUE(result.cyclic_classes.empty());
  EXPECT_EQ(result.cyclic_classes_count, 0);
}

TEST_F(StaticFieldDependencyGraphTest, test_single_class_no_dependencies) {
  // Create a class with a clinit that doesn't depend on anything
  auto* cls = make_class_with_clinit("LTestClassA;", R"(
    (method (public static constructor) "LTestClassA;.<clinit>:()V"
      (
        (const v0 42)
        (sput v0 "LTestClassA;.value:I")
        (return-void)
      )
    )
  )");

  // Manually add the class without dependencies
  UnorderedMap<DexMethod*, DexClass*> candidates;
  auto* clinit = cls->get_clinit();
  candidates.emplace(clinit, cls);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 1);
  EXPECT_TRUE(graph.contains(cls));

  auto result = graph.topological_sort();
  EXPECT_EQ(result.ordered_classes.size(), 1);
  EXPECT_EQ(result.ordered_classes[0], cls);
  EXPECT_TRUE(result.cyclic_classes.empty());
}

TEST_F(StaticFieldDependencyGraphTest, test_add_dependency) {
  auto* classA = make_class_with_clinit("LDepClassA;", R"(
    (method (public static constructor) "LDepClassA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LDepClassA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LDepClassB;", R"(
    (method (public static constructor) "LDepClassB;.<clinit>:()V"
      (
        (const v0 2)
        (sput v0 "LDepClassB;.value:I")
        (return-void)
      )
    )
  )");

  StaticFieldDependencyGraph graph;

  // B depends on A (A must be initialized before B)
  add_dep(graph, classB, classA);

  EXPECT_EQ(graph.size(), 2);
  EXPECT_TRUE(graph.contains(classA));
  EXPECT_TRUE(graph.contains(classB));

  // Check dependencies
  const auto& b_deps = graph.get_dependencies(classB);
  EXPECT_EQ(b_deps.size(), 1);
  EXPECT_TRUE(b_deps.count(classA) > 0);

  // Check dependents (reverse)
  const auto& a_dependents = graph.get_dependents(classA);
  EXPECT_EQ(a_dependents.size(), 1);
  EXPECT_TRUE(a_dependents.count(classB) > 0);

  // A has no dependencies
  const auto& a_deps = graph.get_dependencies(classA);
  EXPECT_TRUE(a_deps.empty());
}

TEST_F(StaticFieldDependencyGraphTest, test_linear_chain) {
  // Create A -> B -> C chain (C depends on B, B depends on A)
  auto* classA = make_class_with_clinit("LChainA;", R"(
    (method (public static constructor) "LChainA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LChainA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LChainB;", R"(
    (method (public static constructor) "LChainB;.<clinit>:()V"
      (
        (sget "LChainA;.value:I")
        (move-result-pseudo v0)
        (sput v0 "LChainB;.value:I")
        (return-void)
      )
    )
  )");

  auto* classC = make_class_with_clinit("LChainC;", R"(
    (method (public static constructor) "LChainC;.<clinit>:()V"
      (
        (sget "LChainB;.value:I")
        (move-result-pseudo v0)
        (sput v0 "LChainC;.value:I")
        (return-void)
      )
    )
  )");

  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(classA->get_clinit(), classA);
  candidates.emplace(classB->get_clinit(), classB);
  candidates.emplace(classC->get_clinit(), classC);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 3);

  auto result = graph.topological_sort();
  EXPECT_EQ(result.ordered_classes.size(), 3);
  EXPECT_TRUE(result.cyclic_classes.empty());

  // Verify order: A before B before C
  EXPECT_TRUE(comes_before(result.ordered_classes, classA, classB));
  EXPECT_TRUE(comes_before(result.ordered_classes, classB, classC));
}

TEST_F(StaticFieldDependencyGraphTest, test_diamond_dependency) {
  // Diamond: A is base, B and C depend on A, D depends on both B and C
  //      A
  //     / \
  //    B   C
  //     \ /
  //      D

  auto* classA = make_class_with_clinit("LDiamondA;", R"(
    (method (public static constructor) "LDiamondA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LDiamondA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LDiamondB;", R"(
    (method (public static constructor) "LDiamondB;.<clinit>:()V"
      (
        (sget "LDiamondA;.value:I")
        (move-result-pseudo v0)
        (sput v0 "LDiamondB;.value:I")
        (return-void)
      )
    )
  )");

  auto* classC = make_class_with_clinit("LDiamondC;", R"(
    (method (public static constructor) "LDiamondC;.<clinit>:()V"
      (
        (sget "LDiamondA;.value:I")
        (move-result-pseudo v0)
        (sput v0 "LDiamondC;.value:I")
        (return-void)
      )
    )
  )");

  auto* classD = make_class_with_clinit("LDiamondD;", R"(
    (method (public static constructor) "LDiamondD;.<clinit>:()V"
      (
        (sget "LDiamondB;.value:I")
        (move-result-pseudo v0)
        (sget "LDiamondC;.value:I")
        (move-result-pseudo v1)
        (add-int v2 v0 v1)
        (sput v2 "LDiamondD;.value:I")
        (return-void)
      )
    )
  )");

  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(classA->get_clinit(), classA);
  candidates.emplace(classB->get_clinit(), classB);
  candidates.emplace(classC->get_clinit(), classC);
  candidates.emplace(classD->get_clinit(), classD);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 4);

  auto result = graph.topological_sort();
  EXPECT_EQ(result.ordered_classes.size(), 4);
  EXPECT_TRUE(result.cyclic_classes.empty());

  // Verify order constraints:
  // A must come before B and C
  EXPECT_TRUE(comes_before(result.ordered_classes, classA, classB));
  EXPECT_TRUE(comes_before(result.ordered_classes, classA, classC));
  // B and C must come before D
  EXPECT_TRUE(comes_before(result.ordered_classes, classB, classD));
  EXPECT_TRUE(comes_before(result.ordered_classes, classC, classD));
}

TEST_F(StaticFieldDependencyGraphTest, test_cycle_detection) {
  // Cycle: A -> B -> C -> A
  // Use simple clinits without cross-class dependencies, then manually
  // add the cycle via add_dependency() to test the cycle detection logic
  auto* classA = make_class_with_clinit("LCycleA;", R"(
    (method (public static constructor) "LCycleA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LCycleA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LCycleB;", R"(
    (method (public static constructor) "LCycleB;.<clinit>:()V"
      (
        (const v0 2)
        (sput v0 "LCycleB;.value:I")
        (return-void)
      )
    )
  )");

  auto* classC = make_class_with_clinit("LCycleC;", R"(
    (method (public static constructor) "LCycleC;.<clinit>:()V"
      (
        (const v0 3)
        (sput v0 "LCycleC;.value:I")
        (return-void)
      )
    )
  )");

  StaticFieldDependencyGraph graph;
  // Create cycle: A depends on C, B depends on A, C depends on B
  // This forms: A -> C -> B -> A
  add_dep(graph, classA, classC);
  add_dep(graph, classC, classB);
  add_dep(graph, classB, classA);

  EXPECT_EQ(graph.size(), 3);

  auto result = graph.topological_sort();

  // All classes should be detected as cyclic
  EXPECT_GT(result.cyclic_classes_count, 0);
  EXPECT_FALSE(result.cyclic_classes.empty());

  // The ordered list should not contain any cyclic classes
  for (auto* cls : result.cyclic_classes) {
    EXPECT_TRUE(std::find(result.ordered_classes.begin(),
                          result.ordered_classes.end(),
                          cls) == result.ordered_classes.end());
  }
}

TEST_F(StaticFieldDependencyGraphTest, test_wide_fan_out) {
  // Base class with 5 dependents (fan-out)
  auto* base = make_class_with_clinit("LFanOutBase;", R"(
    (method (public static constructor) "LFanOutBase;.<clinit>:()V"
      (
        (const v0 100)
        (sput v0 "LFanOutBase;.value:I")
        (return-void)
      )
    )
  )");

  std::vector<DexClass*> children;
  for (int i = 1; i <= 5; i++) {
    std::string name = "LFanOutChild" + std::to_string(i) + ";";
    std::string code = R"(
      (method (public static constructor) ")" +
                       name + R"(.<clinit>:()V"
        (
          (sget "LFanOutBase;.value:I")
          (move-result-pseudo v0)
          (sput v0 ")" +
                       name + R"(.value:I")
          (return-void)
        )
      )
    )";
    children.push_back(make_class_with_clinit(name, code));
  }

  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(base->get_clinit(), base);
  for (auto* child : children) {
    candidates.emplace(child->get_clinit(), child);
  }

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 6); // base + 5 children

  auto result = graph.topological_sort();
  EXPECT_EQ(result.ordered_classes.size(), 6);
  EXPECT_TRUE(result.cyclic_classes.empty());

  // Base must come before all children
  for (auto* child : children) {
    EXPECT_TRUE(comes_before(result.ordered_classes, base, child));
  }
}

TEST_F(StaticFieldDependencyGraphTest, test_invoke_static_dependency) {
  // Test that invoke-static creates a dependency
  auto* classA = make_class_with_clinit("LInvokeA;", R"(
    (method (public static constructor) "LInvokeA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LInvokeA;.value:I")
        (return-void)
      )
    )
  )");

  // Add a static method to classA
  auto* helper_method = assembler::method_from_string(R"(
    (method (public static) "LInvokeA;.helper:()I"
      (
        (sget "LInvokeA;.value:I")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");
  classA->add_method(helper_method);

  auto* classB = make_class_with_clinit("LInvokeB;", R"(
    (method (public static constructor) "LInvokeB;.<clinit>:()V"
      (
        (invoke-static () "LInvokeA;.helper:()I")
        (move-result v0)
        (sput v0 "LInvokeB;.value:I")
        (return-void)
      )
    )
  )");

  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(classA->get_clinit(), classA);
  candidates.emplace(classB->get_clinit(), classB);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 2);

  // B should depend on A due to invoke-static
  const auto& b_deps = graph.get_dependencies(classB);
  EXPECT_EQ(b_deps.size(), 1);
  EXPECT_TRUE(b_deps.count(classA) > 0);

  auto result = graph.topological_sort();
  EXPECT_TRUE(comes_before(result.ordered_classes, classA, classB));
}

TEST_F(StaticFieldDependencyGraphTest, test_new_instance_dependency) {
  // Test that new-instance creates a dependency
  auto* classA = make_class_with_clinit("LNewInstA;", R"(
    (method (public static constructor) "LNewInstA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LNewInstA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LNewInstB;", R"(
    (method (public static constructor) "LNewInstB;.<clinit>:()V"
      (
        (new-instance "LNewInstA;")
        (move-result-pseudo-object v0)
        (const v1 1)
        (sput v1 "LNewInstB;.value:I")
        (return-void)
      )
    )
  )");

  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(classA->get_clinit(), classA);
  candidates.emplace(classB->get_clinit(), classB);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 2);

  // B should depend on A due to new-instance
  const auto& b_deps = graph.get_dependencies(classB);
  EXPECT_EQ(b_deps.size(), 1);
  EXPECT_TRUE(b_deps.count(classA) > 0);

  auto result = graph.topological_sort();
  EXPECT_TRUE(comes_before(result.ordered_classes, classA, classB));
}

TEST_F(StaticFieldDependencyGraphTest, test_mixed_cyclic_and_non_cyclic) {
  // Create a mix: E is independent, A/B/C form a cycle
  // Use simple clinits, then manually add dependencies
  auto* classA = make_class_with_clinit("LMixedCycleA;", R"(
    (method (public static constructor) "LMixedCycleA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LMixedCycleA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LMixedCycleB;", R"(
    (method (public static constructor) "LMixedCycleB;.<clinit>:()V"
      (
        (const v0 2)
        (sput v0 "LMixedCycleB;.value:I")
        (return-void)
      )
    )
  )");

  auto* classC = make_class_with_clinit("LMixedCycleC;", R"(
    (method (public static constructor) "LMixedCycleC;.<clinit>:()V"
      (
        (const v0 3)
        (sput v0 "LMixedCycleC;.value:I")
        (return-void)
      )
    )
  )");

  auto* classE = make_class_with_clinit("LMixedIndep;", R"(
    (method (public static constructor) "LMixedIndep;.<clinit>:()V"
      (
        (const v0 999)
        (sput v0 "LMixedIndep;.value:I")
        (return-void)
      )
    )
  )");

  StaticFieldDependencyGraph graph;
  // Create cycle: A -> C -> B -> A
  add_dep(graph, classA, classC);
  add_dep(graph, classC, classB);
  add_dep(graph, classB, classA);
  // E is independent - just add it to the graph with no dependencies
  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(classE->get_clinit(), classE);
  graph.build(candidates);

  EXPECT_EQ(graph.size(), 4);

  auto result = graph.topological_sort();

  // E should be in ordered list (no cycle involvement)
  EXPECT_TRUE(std::find(result.ordered_classes.begin(),
                        result.ordered_classes.end(),
                        classE) != result.ordered_classes.end());

  // Cyclic classes should not be in ordered list
  EXPECT_GT(result.cyclic_classes.size(), 0);
}

TEST_F(StaticFieldDependencyGraphTest, test_contains_and_get_all_classes) {
  auto* classA = make_class_with_clinit("LContainsTestA;", R"(
    (method (public static constructor) "LContainsTestA;.<clinit>:()V"
      (
        (const v0 1)
        (sput v0 "LContainsTestA;.value:I")
        (return-void)
      )
    )
  )");

  auto* classB = make_class_with_clinit("LContainsTestB;", R"(
    (method (public static constructor) "LContainsTestB;.<clinit>:()V"
      (
        (const v0 2)
        (sput v0 "LContainsTestB;.value:I")
        (return-void)
      )
    )
  )");

  auto* classC = make_class_with_clinit("LContainsTestC;", R"(
    (method (public static constructor) "LContainsTestC;.<clinit>:()V"
      (
        (const v0 3)
        (sput v0 "LContainsTestC;.value:I")
        (return-void)
      )
    )
  )");

  StaticFieldDependencyGraph graph;
  add_dep(graph, classB, classA);

  EXPECT_TRUE(graph.contains(classA));
  EXPECT_TRUE(graph.contains(classB));
  EXPECT_FALSE(graph.contains(classC));

  const auto& all_classes = graph.get_all_classes();
  EXPECT_EQ(all_classes.size(), 2);
  EXPECT_TRUE(all_classes.count(classA) > 0);
  EXPECT_TRUE(all_classes.count(classB) > 0);
}

TEST_F(StaticFieldDependencyGraphTest, MultipleDiamonds) {
  // Test: Multiple overlapping diamond patterns
  //       A <- B, A <- C
  //       B <- D, C <- D
  //       D <- E, D <- F
  //       E <- G, F <- G
  StaticFieldDependencyGraph graph;
  auto* classA = create_class("LMultiDiamondA;");
  auto* classB = create_class("LMultiDiamondB;");
  auto* classC = create_class("LMultiDiamondC;");
  auto* classD = create_class("LMultiDiamondD;");
  auto* classE = create_class("LMultiDiamondE;");
  auto* classF = create_class("LMultiDiamondF;");
  auto* classG = create_class("LMultiDiamondG;");

  add_dep(graph, classB, classA); // B depends on A
  add_dep(graph, classC, classA); // C depends on A
  add_dep(graph, classD, classB); // D depends on B
  add_dep(graph, classD, classC); // D depends on C
  add_dep(graph, classE, classD); // E depends on D
  add_dep(graph, classF, classD); // F depends on D
  add_dep(graph, classG, classE); // G depends on E
  add_dep(graph, classG, classF); // G depends on F

  EXPECT_EQ(graph.size(), 7u);

  auto result = graph.topological_sort();

  EXPECT_EQ(result.ordered_classes.size(), 7u);
  EXPECT_TRUE(result.cyclic_classes.empty());
  EXPECT_EQ(result.cyclic_classes_count, 0u);

  // Helper to find position
  auto find_pos = [&](DexClass* cls) {
    return std::distance(result.ordered_classes.begin(),
                         std::find(result.ordered_classes.begin(),
                                   result.ordered_classes.end(), cls));
  };

  // Verify ordering constraints
  EXPECT_LT(find_pos(classA), find_pos(classB)); // A before B
  EXPECT_LT(find_pos(classA), find_pos(classC)); // A before C
  EXPECT_LT(find_pos(classB), find_pos(classD)); // B before D
  EXPECT_LT(find_pos(classC), find_pos(classD)); // C before D
  EXPECT_LT(find_pos(classD), find_pos(classE)); // D before E
  EXPECT_LT(find_pos(classD), find_pos(classF)); // D before F
  EXPECT_LT(find_pos(classE), find_pos(classG)); // E before G
  EXPECT_LT(find_pos(classF), find_pos(classG)); // F before G
}

TEST_F(StaticFieldDependencyGraphTest, test_field_access_through_subclass) {
  // Regression test: When a static field declared in Parent is accessed through
  // a subclass reference (Child.parentField), the dependency should be tracked
  // to the declaring class (Parent), not the reference class (Child).
  //
  // This matters because the JVM triggers the clinit of the declaring class
  // when accessing a static field, regardless of which class is used in the
  // bytecode reference.

  // Create parent class with a static field
  auto* parent_type = DexType::make_type("LSubclassFieldParent;");
  ClassCreator parent_creator(parent_type);
  parent_creator.set_super(type::java_lang_Object());
  auto* parent_field =
      DexField::make_field("LSubclassFieldParent;.parentValue:I")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC);
  parent_creator.add_field(parent_field);
  auto* parent_clinit = assembler::method_from_string(R"(
    (method (public static constructor) "LSubclassFieldParent;.<clinit>:()V"
      (
        (const v0 42)
        (sput v0 "LSubclassFieldParent;.parentValue:I")
        (return-void)
      )
    )
  )");
  parent_creator.add_method(parent_clinit);
  auto* parent_class = parent_creator.create();

  // Create child class that extends parent (no clinit of its own for
  // simplicity)
  auto* child_type = DexType::make_type("LSubclassFieldChild;");
  ClassCreator child_creator(child_type);
  child_creator.set_super(parent_type);
  // We need to create the child class so the field reference can resolve
  // through it, but we don't use the returned pointer directly
  child_creator.create();

  // Create a third class that accesses parent's field THROUGH the child class
  // reference. The bytecode uses "LSubclassFieldChild;.parentValue:I" but the
  // field is actually declared in LSubclassFieldParent;
  auto* reader_type = DexType::make_type("LSubclassFieldReader;");
  ClassCreator reader_creator(reader_type);
  reader_creator.set_super(type::java_lang_Object());
  auto* reader_field = DexField::make_field("LSubclassFieldReader;.value:I")
                           ->make_concrete(ACC_STATIC | ACC_PUBLIC);
  reader_creator.add_field(reader_field);
  auto* reader_clinit = assembler::method_from_string(R"(
    (method (public static constructor) "LSubclassFieldReader;.<clinit>:()V"
      (
        ; Access parent's field through child class reference
        (sget "LSubclassFieldChild;.parentValue:I")
        (move-result-pseudo v0)
        (sput v0 "LSubclassFieldReader;.value:I")
        (return-void)
      )
    )
  )");
  reader_creator.add_method(reader_clinit);
  auto* reader_class = reader_creator.create();

  // Build the dependency graph with only Parent and Reader as candidates
  // (Child is NOT a candidate - this simulates a common scenario where
  // subclasses are not hot/not candidates)
  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(parent_class->get_clinit(), parent_class);
  candidates.emplace(reader_class->get_clinit(), reader_class);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  // Reader should depend on Parent (the declaring class), not Child
  // (the reference class which is not even a candidate)
  const auto& reader_deps = graph.get_dependencies(reader_class);
  EXPECT_EQ(reader_deps.size(), 1);
  EXPECT_TRUE(reader_deps.count(parent_class) > 0)
      << "Reader should depend on Parent (the declaring class), not Child";

  // Verify topological sort puts Parent before Reader
  auto result = graph.topological_sort();
  EXPECT_EQ(result.ordered_classes.size(), 2);
  EXPECT_TRUE(result.cyclic_classes.empty());
  EXPECT_TRUE(comes_before(result.ordered_classes, parent_class, reader_class))
      << "Parent should come before Reader in topological order";
}

TEST_F(StaticFieldDependencyGraphTest,
       test_transitive_dependency_via_constructor) {
  // Regression test: When a clinit creates a new instance of class B, and B's
  // constructor reads a static field from candidate class C, then the clinit
  // transitively depends on C.
  //
  // Scenario:
  // - Class A (candidate) clinit creates: new B()
  // - Class B (non-candidate) constructor reads: C.staticField
  // - Class C (candidate) has the static field
  //
  // Result: A must depend on C, even though A doesn't directly access C.

  // Create class C with a static field (this is a candidate)
  auto* classC_type = DexType::make_type("LTransitiveC;");
  ClassCreator classC_creator(classC_type);
  classC_creator.set_super(type::java_lang_Object());
  auto* classC_field = DexField::make_field("LTransitiveC;.value:I")
                           ->make_concrete(ACC_STATIC | ACC_PUBLIC);
  classC_creator.add_field(classC_field);
  auto* classC_clinit = assembler::method_from_string(R"(
    (method (public static constructor) "LTransitiveC;.<clinit>:()V"
      (
        (const v0 100)
        (sput v0 "LTransitiveC;.value:I")
        (return-void)
      )
    )
  )");
  classC_creator.add_method(classC_clinit);
  auto* classC = classC_creator.create();

  // Create class B (non-candidate) whose constructor reads C.value
  auto* classB_type = DexType::make_type("LTransitiveB;");
  ClassCreator classB_creator(classB_type);
  classB_creator.set_super(type::java_lang_Object());
  auto* classB_field =
      DexField::make_field("LTransitiveB;.bValue:I")->make_concrete(ACC_PUBLIC);
  classB_creator.add_field(classB_field);
  auto* classB_ctor = assembler::method_from_string(R"(
    (method (public constructor) "LTransitiveB;.<init>:()V"
      (
        (load-param-object v1)
        (invoke-direct (v1) "Ljava/lang/Object;.<init>:()V")
        ; Read static field from C
        (sget "LTransitiveC;.value:I")
        (move-result-pseudo v0)
        (iput v0 v1 "LTransitiveB;.bValue:I")
        (return-void)
      )
    )
  )");
  classB_creator.add_method(classB_ctor);
  classB_creator.create();

  // Create class A (candidate) whose clinit creates new B()
  auto* classA_type = DexType::make_type("LTransitiveA;");
  ClassCreator classA_creator(classA_type);
  classA_creator.set_super(type::java_lang_Object());
  auto* classA_field =
      DexField::make_field("LTransitiveA;.instance:LTransitiveB;")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC);
  classA_creator.add_field(classA_field);
  auto* classA_clinit = assembler::method_from_string(R"(
    (method (public static constructor) "LTransitiveA;.<clinit>:()V"
      (
        ; Create new B() - B's constructor reads C.value
        (new-instance "LTransitiveB;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "LTransitiveB;.<init>:()V")
        (sput-object v0 "LTransitiveA;.instance:LTransitiveB;")
        (return-void)
      )
    )
  )");
  classA_creator.add_method(classA_clinit);
  auto* classA = classA_creator.create();

  // Build dependency graph with A and C as candidates (B is not a candidate)
  UnorderedMap<DexMethod*, DexClass*> candidates;
  candidates.emplace(classA->get_clinit(), classA);
  candidates.emplace(classC->get_clinit(), classC);

  StaticFieldDependencyGraph graph;
  graph.build(candidates);

  // A should transitively depend on C (through B's constructor)
  const auto& a_deps = graph.get_dependencies(classA);
  EXPECT_TRUE(a_deps.count(classC) > 0)
      << "A should transitively depend on C (via B's constructor reading "
         "C.value)";

  // Verify topological sort puts C before A
  auto result = graph.topological_sort();
  EXPECT_EQ(result.ordered_classes.size(), 2);
  EXPECT_TRUE(result.cyclic_classes.empty());
  EXPECT_TRUE(comes_before(result.ordered_classes, classC, classA))
      << "C should come before A in topological order";
}

TEST_F(StaticFieldDependencyGraphTest, test_self_loop) {
  auto* classA = create_class("LSelfLoopA;");

  StaticFieldDependencyGraph graph;
  add_dep(graph, classA, classA);

  EXPECT_EQ(graph.size(), 1);

  auto result = graph.topological_sort();

  EXPECT_EQ(result.cyclic_classes.size(), 1);
  EXPECT_EQ(result.ordered_classes.size(), 0);
}

TEST_F(StaticFieldDependencyGraphTest, test_cycle_with_tail) {
  // D -> A -> B -> C -> A
  // A, B, C form a cycle. D depends on cyclic node A.
  auto* classA = create_class("LTailCycleA;");
  auto* classB = create_class("LTailCycleB;");
  auto* classC = create_class("LTailCycleC;");
  auto* classD = create_class("LTailCycleD;");

  StaticFieldDependencyGraph graph;
  add_dep(graph, classA, classB);
  add_dep(graph, classB, classC);
  add_dep(graph, classC, classA);
  add_dep(graph, classD, classA);

  auto result = graph.topological_sort();

  // A, B, C are in a cycle
  EXPECT_GE(result.cyclic_classes.size(), 3);

  // D depends on cyclic A. The current algorithm places D in
  // ordered_classes because cycle unwinding stops at the cycle root.
  // This is safe: ClinitBatchingPass only transforms ordered_classes,
  // and cyclic classes (A, B, C) are excluded — so D's __initStatics$
  // will call A's clinit normally at runtime (A still has its clinit).
  auto d_in_ordered = std::find(result.ordered_classes.begin(),
                                result.ordered_classes.end(), classD);
  EXPECT_NE(d_in_ordered, result.ordered_classes.end())
      << "D ends up in ordered list (tail of cycle, not itself cyclic)";
}

TEST_F(StaticFieldDependencyGraphTest, test_multiple_independent_cycles) {
  // Two separate cycles: A <-> B, and C <-> D
  auto* classA = create_class("LMultiCycleA;");
  auto* classB = create_class("LMultiCycleB;");
  auto* classC = create_class("LMultiCycleC;");
  auto* classD = create_class("LMultiCycleD;");
  StaticFieldDependencyGraph graph;
  // Cycle 1: A <-> B
  add_dep(graph, classA, classB);
  add_dep(graph, classB, classA);
  // Cycle 2: C <-> D
  add_dep(graph, classC, classD);
  add_dep(graph, classD, classC);

  EXPECT_EQ(graph.size(), 4);

  auto result = graph.topological_sort();

  // All 4 classes should be cyclic
  EXPECT_EQ(result.cyclic_classes.size(), 4);
  EXPECT_EQ(result.ordered_classes.size(), 0);
}

TEST_F(StaticFieldDependencyGraphTest, test_non_cyclic_with_cyclic_neighbors) {
  // Graph: E -> A, A <-> B, C -> D (linear)
  // A <-> B is cyclic. E depends on cyclic A (tail node).
  // C -> D is non-cyclic, so both C and D should be ordered (D before C).
  auto* classA = create_class("LNeighborA;");
  auto* classB = create_class("LNeighborB;");
  auto* classC = create_class("LNeighborC;");
  auto* classD = create_class("LNeighborD;");
  auto* classE = create_class("LNeighborE;");

  StaticFieldDependencyGraph graph;
  add_dep(graph, classA, classB);
  add_dep(graph, classB, classA);
  add_dep(graph, classE, classA);
  add_dep(graph, classC, classD);

  EXPECT_EQ(graph.size(), 5);

  auto result = graph.topological_sort();

  // A and B are cyclic
  auto a_cyclic = std::find(result.cyclic_classes.begin(),
                            result.cyclic_classes.end(), classA);
  auto b_cyclic = std::find(result.cyclic_classes.begin(),
                            result.cyclic_classes.end(), classB);
  EXPECT_NE(a_cyclic, result.cyclic_classes.end());
  EXPECT_NE(b_cyclic, result.cyclic_classes.end());

  // C and D should be in ordered list, D before C
  auto c_ordered = std::find(result.ordered_classes.begin(),
                             result.ordered_classes.end(), classC);
  auto d_ordered = std::find(result.ordered_classes.begin(),
                             result.ordered_classes.end(), classD);
  EXPECT_NE(c_ordered, result.ordered_classes.end())
      << "C should be in ordered list (non-cyclic)";
  EXPECT_NE(d_ordered, result.ordered_classes.end())
      << "D should be in ordered list (non-cyclic)";
  EXPECT_TRUE(comes_before(result.ordered_classes, classD, classC))
      << "D should come before C in topological order";

  // E depends on cyclic A — same tail behavior as test_cycle_with_tail.
  // E ends up in ordered_classes because A retains its clinit.
  auto e_ordered = std::find(result.ordered_classes.begin(),
                             result.ordered_classes.end(), classE);
  EXPECT_NE(e_ordered, result.ordered_classes.end())
      << "E should be in ordered list (tail of cycle)";
}
