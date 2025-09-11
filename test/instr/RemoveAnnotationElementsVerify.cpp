/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexAnnotation.h"
#include "Show.h"
#include "verify/VerifyUtil.h"

using namespace testing;

TEST_F(PostVerify, VerifyAnnotationElements) {
  auto* foo_anno = find_class_named(classes, "LFooAnno;");
  auto* inner_anno = find_class_named(classes, "LInnerAnno;");
  auto* bar_anno = find_class_named(classes, "LBarAnno;");
  auto* foo_cls = find_class_named(classes, "LFooClass;");

  // Verify the annotation classes got chopped down by RMU
  {
    EXPECT_NE(foo_anno, nullptr) << "Did not find LFooAnno;!";
    EXPECT_EQ(foo_anno->get_vmethods().size(), 2);
    std::vector<std::string> vmethod_names;
    for (auto* m : foo_anno->get_vmethods()) {
      vmethod_names.push_back(m->str_copy());
    }
    EXPECT_THAT(vmethod_names, ::testing::UnorderedElementsAre("x", "inner"));
  }

  {
    EXPECT_NE(inner_anno, nullptr) << "Did not find LBarAnno;!";
    EXPECT_EQ(inner_anno->get_vmethods().size(), 1);
    auto* vmethod = *inner_anno->get_vmethods().begin();
    EXPECT_STREQ("q", vmethod->get_name()->c_str());
  }

  {
    EXPECT_NE(bar_anno, nullptr) << "Did not find LBarAnno;!";
    EXPECT_EQ(bar_anno->get_vmethods().size(), 1);
    auto* vmethod = *bar_anno->get_vmethods().begin();
    EXPECT_STREQ("a", vmethod->get_name()->c_str());
  }

  // Check the annotations on the class, field and method.
  EXPECT_NE(foo_cls, nullptr) << "Did not find LFooClass;!";
  EXPECT_EQ(foo_cls->get_ifields().size(), 1);
  EXPECT_EQ(foo_cls->get_vmethods().size(), 1);

  auto verify_annotation_set_has =
      [&](DexAnnotationSet* aset, DexType* expected_anno_type,
          const std::vector<std::string>& expected_anno_members,
          std::unordered_map<std::string, DexEncodedValue*>* out_values =
              nullptr) {
        EXPECT_NE(aset, nullptr);
        bool found{false};
        for (auto& anno : aset->get_annotations()) {
          if (anno->type() == expected_anno_type) {
            found = true;
            std::vector<std::string> actual_strings;
            for (const auto& anno_element : anno->anno_elems()) {
              auto str_copy = anno_element.string->str_copy();
              actual_strings.push_back(str_copy);
              if (out_values != nullptr) {
                out_values->emplace(str_copy, anno_element.encoded_value.get());
              }
            }
            EXPECT_THAT(
                actual_strings,
                ::testing::UnorderedElementsAreArray(expected_anno_members));
          }
        }
        EXPECT_TRUE(found) << "Member should be annotated with "
                           << show(expected_anno_type);
      };

  std::unordered_map<std::string, DexEncodedValue*> values_to_check;
  verify_annotation_set_has(foo_cls->get_anno_set(), foo_anno->get_type(),
                            {"x", "inner"}, &values_to_check);
  // Also check the inner annotation
  EXPECT_EQ(values_to_check.size(), 2);
  auto* inner_instance = values_to_check.at("inner");
  EXPECT_EQ(inner_instance->evtype(), DEVT_ANNOTATION);
  const auto& inner_instance_elemenets =
      dynamic_cast<DexEncodedValueAnnotation*>(inner_instance)->annotations();
  EXPECT_EQ(inner_instance_elemenets.size(), 1);
  auto should_be_q = inner_instance_elemenets.begin()->string->str_copy();
  EXPECT_STREQ(should_be_q.c_str(), "q");
  // Simple annos with no nested annotations.
  auto* s = *foo_cls->get_ifields().begin();
  verify_annotation_set_has(s->get_anno_set(), bar_anno->get_type(), {"a"});
  auto* m = *foo_cls->get_vmethods().begin();
  verify_annotation_set_has(m->get_anno_set(), bar_anno->get_type(), {"a"});
}
