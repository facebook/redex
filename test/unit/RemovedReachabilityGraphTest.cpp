/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "Creators.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "Reachability.h"
#include "RedexTest.h"
#include "Show.h"
#include "TypeUtil.h"

using namespace reachability;

namespace {

struct Fixture {
  DexClass* cls;
  DexField* field;
  DexMethod* method;
};

Fixture create_class(const std::string& descriptor, bool external = false) {
  ClassCreator creator(DexType::make_type(descriptor));
  creator.set_super(type::java_lang_Object());

  auto* field =
      DexField::make_field(descriptor + ".f:I")->make_concrete(ACC_PUBLIC);
  creator.add_field(field);

  auto* method = DexMethod::make_method(descriptor + ".m:()V")
                     ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
  creator.add_method(method);

  if (external) {
    creator.set_external();
  }
  return {creator.create(), field, method};
}

DexField* add_field(ClassCreator& creator, const std::string& spec) {
  auto* field = DexField::make_field(spec)->make_concrete(ACC_PUBLIC);
  creator.add_field(field);
  return field;
}

DexMethod* add_method(ClassCreator& creator,
                      const std::string& spec,
                      const std::string& code) {
  auto* method = DexMethod::make_method(spec)->make_concrete(
      ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
  method->set_code(assembler::ircode_from_string(code));
  creator.add_method(method);
  return method;
}

void annotate(DexClass* cls, const std::vector<std::string>& descriptors) {
  auto anno_set = std::make_unique<DexAnnotationSet>();
  for (const auto& descriptor : descriptors) {
    anno_set->add_annotation(std::make_unique<DexAnnotation>(
        DexType::make_type(descriptor), DexAnnotationVisibility::DAV_RUNTIME));
  }
  always_assert(cls->attach_annotation_set(std::move(anno_set)));
}

// The removed graph only ever contains these three kinds; anything else shows
// up as the sentinel and fails the assertions that scan for it.
std::string label(const ReachableObject& obj) {
  if (obj.type == ReachableObjectType::CLASS) {
    return show(obj.cls);
  }
  if (obj.type == ReachableObjectType::FIELD) {
    return show(obj.field);
  }
  if (obj.type == ReachableObjectType::METHOD) {
    return show(obj.method);
  }
  return "<unexpected node kind>";
}

// Predecessor labels of a node that is expected to be in the graph. Returns a
// sentinel rather than an empty set for a missing node, so that "absent" cannot
// be mistaken for "present with no predecessors" -- the difference between a
// dropped node and a root.
std::set<std::string> preds_of(const ReachableObjectGraph& graph,
                               const ReachableObject& node) {
  if (graph.count(node) == 0u) {
    return {"<node missing from graph>"};
  }
  std::set<std::string> result;
  for (const auto& pred : UnorderedIterable(graph.at_unsafe(node))) {
    result.insert(label(pred));
  }
  return result;
}

} // namespace

class RemovedReachabilityGraphTest : public RedexTest {};

TEST_F(RemovedReachabilityGraphTest, NodeSetCoversEveryPredicateCombination) {
  auto removed_unmarked = create_class("LRemovedUnmarkedMember;");
  auto removed_marked = create_class("LRemovedMarkedMember;");
  auto retained_unmarked = create_class("LRetainedUnmarkedMember;");
  auto retained_marked = create_class("LRetainedMarkedMember;");
  auto external = create_class("LExternal;", /* external */ true);
  auto out_of_scope = create_class("LOutOfScope;");

  ReachableObjects reachables;
  reachables.mark(retained_unmarked.cls);
  reachables.mark(retained_marked.cls);
  reachables.mark(retained_marked.field);
  reachables.mark(retained_marked.method);
  // A marked member of an unmarked class. `sweep` deletes it along with its
  // class, so it has to be a node despite the mark.
  reachables.mark(removed_marked.field);
  reachables.mark(removed_marked.method);

  // `build_class_scope` never yields an external class; this one is here so the
  // external check is exercised independently of the in-scope check.
  Scope scope{removed_unmarked.cls, removed_marked.cls, retained_unmarked.cls,
              retained_marked.cls, external.cls};

  auto graph = compute_removed_reachability_graph(scope, reachables);

  auto has = [&graph](const auto* obj) {
    return graph.count(ReachableObject(obj)) != 0u;
  };

  EXPECT_TRUE(has(removed_unmarked.cls));
  EXPECT_TRUE(has(removed_unmarked.field));
  EXPECT_TRUE(has(removed_unmarked.method));

  EXPECT_TRUE(has(removed_marked.cls));
  EXPECT_TRUE(has(removed_marked.field));
  EXPECT_TRUE(has(removed_marked.method));

  EXPECT_FALSE(has(retained_unmarked.cls));
  EXPECT_TRUE(has(retained_unmarked.field));
  EXPECT_TRUE(has(retained_unmarked.method));

  EXPECT_FALSE(has(retained_marked.cls));
  EXPECT_FALSE(has(retained_marked.field));
  EXPECT_FALSE(has(retained_marked.method));

  EXPECT_FALSE(has(external.cls));
  EXPECT_FALSE(has(external.field));
  EXPECT_FALSE(has(external.method));

  EXPECT_FALSE(has(out_of_scope.cls));
  EXPECT_FALSE(has(out_of_scope.field));
  EXPECT_FALSE(has(out_of_scope.method));
}

TEST_F(RemovedReachabilityGraphTest, ContainmentAndDirectReferences) {
  ClassCreator owner_creator(DexType::make_type("LOwner;"));
  owner_creator.set_super(type::java_lang_Object());
  auto* owner_field = add_field(owner_creator, "LOwner;.f:LFieldType;");
  auto* owner_method = add_method(owner_creator, "LOwner;.m:()V", R"(
    (
      (invoke-static () "LTarget;.tm:()V")
      (sget "LTarget;.t:I")
      (move-result-pseudo v0)
      (new-instance "LTarget;")
      (move-result-pseudo-object v1)
      (invoke-static () "LRetained;.keep:()V")
      (return-void)
    )
  )");
  auto* owner = owner_creator.create();

  ClassCreator field_type_creator(DexType::make_type("LFieldType;"));
  field_type_creator.set_super(type::java_lang_Object());
  auto* field_type = field_type_creator.create();

  ClassCreator target_creator(DexType::make_type("LTarget;"));
  target_creator.set_super(type::java_lang_Object());
  auto* target_field = add_field(target_creator, "LTarget;.t:I");
  auto* target_method =
      add_method(target_creator, "LTarget;.tm:()V", "((return-void))");
  auto* target = target_creator.create();

  ClassCreator retained_creator(DexType::make_type("LRetained;"));
  retained_creator.set_super(type::java_lang_Object());
  auto* retained_method =
      add_method(retained_creator, "LRetained;.keep:()V", "((return-void))");
  auto* retained = retained_creator.create();

  ReachableObjects reachables;
  reachables.mark(retained);
  reachables.mark(retained_method);

  Scope scope{owner, field_type, target, retained};
  auto graph = compute_removed_reachability_graph(scope, reachables);

  // A removed class is a root: containment points away from it, and the trivial
  // member-to-own-class reference is suppressed.
  EXPECT_EQ(preds_of(graph, ReachableObject(owner)), std::set<std::string>{});

  // Containment only -- neither member is referenced by anything else.
  EXPECT_EQ(preds_of(graph, ReachableObject(owner_field)),
            std::set<std::string>{"LOwner;"});
  EXPECT_EQ(preds_of(graph, ReachableObject(owner_method)),
            std::set<std::string>{"LOwner;"});

  // The field's declared type. `DexField::gather_types` alone would miss this.
  EXPECT_EQ(preds_of(graph, ReachableObject(field_type)),
            std::set<std::string>{"LOwner;.f:LFieldType;"});

  EXPECT_EQ(preds_of(graph, ReachableObject(target)),
            std::set<std::string>{"LOwner;.m:()V"});
  EXPECT_EQ(preds_of(graph, ReachableObject(target_field)),
            (std::set<std::string>{"LOwner;.m:()V", "LTarget;"}));
  EXPECT_EQ(preds_of(graph, ReachableObject(target_method)),
            (std::set<std::string>{"LOwner;.m:()V", "LTarget;"}));
}

TEST_F(RemovedReachabilityGraphTest, ArrayAndInheritedMethodResolution) {
  ClassCreator base_creator(DexType::make_type("LBase;"));
  base_creator.set_super(type::java_lang_Object());
  auto* base_method =
      add_method(base_creator, "LBase;.bm:()V", "((return-void))");
  auto* base = base_creator.create();

  ClassCreator derived_creator(DexType::make_type("LDerived;"));
  derived_creator.set_super(DexType::make_type("LBase;"));
  auto* derived = derived_creator.create();

  ClassCreator elem_creator(DexType::make_type("LElem;"));
  elem_creator.set_super(type::java_lang_Object());
  auto* elem = elem_creator.create();

  ClassCreator caller_creator(DexType::make_type("LCaller;"));
  caller_creator.set_super(type::java_lang_Object());
  add_field(caller_creator, "LCaller;.arr:[LElem;");
  // The invoked ref is declared on LDerived;, which has no such method, so
  // `as_def()` is null and only hierarchy resolution finds LBase;.bm.
  add_method(caller_creator, "LCaller;.c:()V", R"(
    (
      (invoke-static () "LDerived;.bm:()V")
      (return-void)
    )
  )");
  auto* caller = caller_creator.create();

  ReachableObjects reachables;
  Scope scope{base, derived, elem, caller};
  auto graph = compute_removed_reachability_graph(scope, reachables);

  // Array element normalization: the field's type is [LElem;.
  EXPECT_EQ(preds_of(graph, ReachableObject(elem)),
            std::set<std::string>{"LCaller;.arr:[LElem;"});

  // Inherited resolution, plus containment from the declaring class.
  EXPECT_EQ(preds_of(graph, ReachableObject(base_method)),
            (std::set<std::string>{"LBase;", "LCaller;.c:()V"}));

  // LDerived; extends LBase;, from the shallow class gather.
  EXPECT_EQ(preds_of(graph, ReachableObject(base)),
            std::set<std::string>{"LDerived;"});
}

TEST_F(RemovedReachabilityGraphTest, AnnotationsTraversedAndStringsExcluded) {
  ClassCreator anno_creator(DexType::make_type("LRemovedAnno;"));
  anno_creator.set_super(type::java_lang_Object());
  auto* anno = anno_creator.create();

  // A real class for the inner-class annotation, so that skipping it is what
  // keeps the edge out rather than the target simply not being removed.
  ClassCreator member_classes_creator(
      DexType::make_type("Ldalvik/annotation/MemberClasses;"));
  member_classes_creator.set_super(type::java_lang_Object());
  auto* member_classes = member_classes_creator.create();

  ClassCreator string_ref_creator(DexType::make_type("LStringRef;"));
  string_ref_creator.set_super(type::java_lang_Object());
  auto* string_ref = string_ref_creator.create();

  ClassCreator annotated_creator(DexType::make_type("LAnnotated;"));
  annotated_creator.set_super(type::java_lang_Object());
  add_method(annotated_creator, "LAnnotated;.s:()V", R"(
    (
      (const-string "LStringRef;")
      (move-result-pseudo-object v0)
      (return-void)
    )
  )");
  auto* annotated = annotated_creator.create();
  annotate(annotated, {"LRemovedAnno;", "Ldalvik/annotation/MemberClasses;"});

  ReachableObjects reachables;
  Scope scope{anno, member_classes, string_ref, annotated};
  auto graph = compute_removed_reachability_graph(scope, reachables);

  // Annotations are traversed through: the referenced entity becomes a CLASS
  // node, and no ANNO node is ever created.
  EXPECT_EQ(preds_of(graph, ReachableObject(anno)),
            std::set<std::string>{"LAnnotated;"});
  for (const auto& entry : UnorderedIterable(graph)) {
    EXPECT_NE(label(entry.first), "<unexpected node kind>");
  }

  // Inner-class annotations are skipped, as they are by the marker.
  EXPECT_EQ(preds_of(graph, ReachableObject(member_classes)),
            std::set<std::string>{});

  // Typelike strings are a documented v1 exclusion.
  EXPECT_EQ(preds_of(graph, ReachableObject(string_ref)),
            std::set<std::string>{});
}

TEST_F(RemovedReachabilityGraphTest, RemovedMembersOfARetainedClassCanCycle) {
  ClassCreator creator(DexType::make_type("LHost;"));
  creator.set_super(type::java_lang_Object());
  auto* a = add_method(creator, "LHost;.a:()V", R"(
    (
      (invoke-static () "LHost;.b:()V")
      (return-void)
    )
  )");
  auto* b = add_method(creator, "LHost;.b:()V", R"(
    (
      (invoke-static () "LHost;.a:()V")
      (return-void)
    )
  )");
  auto* r = add_method(creator, "LHost;.r:()V", R"(
    (
      (invoke-static () "LHost;.r:()V")
      (return-void)
    )
  )");
  auto* host = creator.create();

  ReachableObjects reachables;
  reachables.mark(host);

  Scope scope{host};
  auto graph = compute_removed_reachability_graph(scope, reachables);

  // The retained owner is not a node, so the two removed methods form a cycle
  // with no zero-predecessor node: a rootless component.
  EXPECT_EQ(preds_of(graph, ReachableObject(a)),
            std::set<std::string>{"LHost;.b:()V"});
  EXPECT_EQ(preds_of(graph, ReachableObject(b)),
            std::set<std::string>{"LHost;.a:()V"});

  // A directly recursive method gets no self-edge, so it stays a root.
  EXPECT_EQ(preds_of(graph, ReachableObject(r)), std::set<std::string>{});
}
