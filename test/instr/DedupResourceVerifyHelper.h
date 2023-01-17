/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "RedexResources.h"
#include "verify/VerifyUtil.h"

namespace {
#define EXPECT_FIELDS_DIFFERENT(a, b)            \
  ({                                             \
    ASSERT_NE(nullptr, (a));                     \
    ASSERT_NE(nullptr, (b));                     \
    EXPECT_NE((a)->get_static_value()->value(),  \
              (b)->get_static_value()->value()); \
  })
#define EXPECT_FIELDS_SAME(a, b)                 \
  ({                                             \
    ASSERT_NE(nullptr, (a));                     \
    ASSERT_NE(nullptr, (b));                     \
    EXPECT_EQ((a)->get_static_value()->value(),  \
              (b)->get_static_value()->value()); \
  })
} // namespace

inline void dedupresource_preverify(const DexClasses& classes,
                                    ResourceTableFile* res_table) {
  auto dimen_cls = find_class_named(classes, "Lcom/facebook/R$dimen;");
  EXPECT_NE(nullptr, dimen_cls);
  auto margin_top = find_sfield_named(*dimen_cls, "margin_top");
  auto padding_left = find_sfield_named(*dimen_cls, "padding_left");
  auto padding_right = find_sfield_named(*dimen_cls, "padding_right");
  auto unused_dimen_1 = find_sfield_named(*dimen_cls, "unused_dimen_1");
  auto unused_dimen_2 = find_sfield_named(*dimen_cls, "unused_dimen_2");
  auto welcome_text_size = find_sfield_named(*dimen_cls, "welcome_text_size");
  auto small = find_sfield_named(*dimen_cls, "small");
  auto medium = find_sfield_named(*dimen_cls, "medium");
  auto medium2 = find_sfield_named(*dimen_cls, "medium2");
  auto foo = find_sfield_named(*dimen_cls, "foo");
  auto bar = find_sfield_named(*dimen_cls, "bar");
  auto far = find_sfield_named(*dimen_cls, "far");
  auto baz = find_sfield_named(*dimen_cls, "baz");
  auto boo = find_sfield_named(*dimen_cls, "boo");

  EXPECT_FIELDS_DIFFERENT(padding_right, padding_left);
  EXPECT_FIELDS_DIFFERENT(padding_right, unused_dimen_2);
  EXPECT_FIELDS_DIFFERENT(padding_left, unused_dimen_2);

  EXPECT_FIELDS_DIFFERENT(medium, medium2);
  EXPECT_FIELDS_DIFFERENT(foo, bar);
  EXPECT_FIELDS_DIFFERENT(far, foo);
  EXPECT_FIELDS_DIFFERENT(baz, boo);

  auto margin_top_ids = res_table->get_res_ids_by_name("margin_top");
  auto welcome_text_size_ids =
      res_table->get_res_ids_by_name("welcome_text_size");
  auto padding_left_ids = res_table->get_res_ids_by_name("padding_left");
  auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
  auto unused_dimen_1_ids = res_table->get_res_ids_by_name("unused_dimen_1");
  auto unused_dimen_2_ids = res_table->get_res_ids_by_name("unused_dimen_2");
  auto small_ids = res_table->get_res_ids_by_name("small");
  auto medium_ids = res_table->get_res_ids_by_name("medium");
  auto medium2_ids = res_table->get_res_ids_by_name("medium2");
  auto foo_ids = res_table->get_res_ids_by_name("foo");
  auto bar_ids = res_table->get_res_ids_by_name("bar");
  auto far_ids = res_table->get_res_ids_by_name("far");
  auto baz_ids = res_table->get_res_ids_by_name("baz");
  auto boo_ids = res_table->get_res_ids_by_name("boo");
  EXPECT_EQ(margin_top_ids.size(), 1);
  EXPECT_EQ(welcome_text_size_ids.size(), 1);
  EXPECT_EQ(padding_left_ids.size(), 1);
  EXPECT_EQ(padding_right_ids.size(), 1);
  EXPECT_EQ(unused_dimen_1_ids.size(), 1);
  EXPECT_EQ(unused_dimen_2_ids.size(), 1);
  EXPECT_EQ(small_ids.size(), 1);
  EXPECT_EQ(medium_ids.size(), 1);
  EXPECT_EQ(medium2_ids.size(), 1);
  EXPECT_EQ(foo_ids.size(), 1);
  EXPECT_EQ(bar_ids.size(), 1);
  EXPECT_EQ(far_ids.size(), 1);
  EXPECT_EQ(baz_ids.size(), 1);
  EXPECT_EQ(boo_ids.size(), 1);

  EXPECT_EQ(margin_top_ids[0], margin_top->get_static_value()->value());
  EXPECT_EQ(welcome_text_size_ids[0],
            welcome_text_size->get_static_value()->value());
  EXPECT_EQ(padding_left_ids[0], padding_left->get_static_value()->value());
  EXPECT_EQ(padding_right_ids[0], padding_right->get_static_value()->value());
  EXPECT_EQ(unused_dimen_1_ids[0], unused_dimen_1->get_static_value()->value());
  EXPECT_EQ(unused_dimen_2_ids[0], unused_dimen_2->get_static_value()->value());
  EXPECT_EQ(small_ids[0], small->get_static_value()->value());
  EXPECT_EQ(medium_ids[0], medium->get_static_value()->value());
  EXPECT_EQ(medium2_ids[0], medium2->get_static_value()->value());
  EXPECT_EQ(foo_ids[0], foo->get_static_value()->value());
  EXPECT_EQ(bar_ids[0], bar->get_static_value()->value());
  EXPECT_EQ(far_ids[0], far->get_static_value()->value());
  EXPECT_EQ(baz_ids[0], baz->get_static_value()->value());
  EXPECT_EQ(boo_ids[0], boo->get_static_value()->value());

  auto dup_theme1_ids = res_table->get_res_ids_by_name("DupTheme1");
  auto dup_theme2_ids = res_table->get_res_ids_by_name("DupTheme2");
  EXPECT_EQ(dup_theme1_ids.size(), 1);
  EXPECT_EQ(dup_theme2_ids.size(), 1);
  auto style_cls = find_class_named(classes, "Lcom/facebook/R$style;");
  auto dup_theme1_field = find_sfield_named(*style_cls, "DupTheme1");
  auto dup_theme2_field = find_sfield_named(*style_cls, "DupTheme2");
  EXPECT_FIELDS_DIFFERENT(dup_theme1_field, dup_theme2_field);

  auto style_not_sorted_ids = res_table->get_res_ids_by_name("StyleNotSorted");
  auto style_sorted_ids = res_table->get_res_ids_by_name("StyleSorted");
  EXPECT_EQ(style_not_sorted_ids.size(), 1);
  EXPECT_EQ(style_sorted_ids.size(), 1);
  auto style_not_sorted_field = find_sfield_named(*style_cls, "StyleNotSorted");
  auto style_sorted_field = find_sfield_named(*style_cls, "StyleSorted");
  EXPECT_FIELDS_DIFFERENT(style_not_sorted_field, style_sorted_field);

  auto theme_different_a_ids =
      res_table->get_res_ids_by_name("ThemeDifferentA");
  auto theme_different_b_ids =
      res_table->get_res_ids_by_name("ThemeDifferentB");
  EXPECT_EQ(theme_different_a_ids.size(), 1);
  EXPECT_EQ(theme_different_b_ids.size(), 1);
  auto theme_different_a_field =
      find_sfield_named(*style_cls, "ThemeDifferentA");
  auto theme_different_b_field =
      find_sfield_named(*style_cls, "ThemeDifferentB");
  EXPECT_FIELDS_DIFFERENT(theme_different_a_field, theme_different_b_field);

  auto same_attribute_a_ids = res_table->get_res_ids_by_name("SameAttributeA");
  auto same_attribute_b_ids = res_table->get_res_ids_by_name("SameAttributeB");
  EXPECT_EQ(same_attribute_a_ids.size(), 1);
  EXPECT_EQ(same_attribute_b_ids.size(), 1);
  auto attr_cls = find_class_named(classes, "Lcom/facebook/R$attr;");
  auto same_attribute_a_field = find_sfield_named(*attr_cls, "SameAttributeA");
  auto same_attribute_b_field = find_sfield_named(*attr_cls, "SameAttributeB");
  EXPECT_FIELDS_DIFFERENT(same_attribute_a_field, same_attribute_b_field);

  // drawable
  auto drawable_cls = find_class_named(classes, "Lcom/facebook/R$drawable;");
  EXPECT_NE(nullptr, drawable_cls);
  auto icon = find_sfield_named(*drawable_cls, "icon");
  auto x_icon = find_sfield_named(*drawable_cls, "x_icon");
  EXPECT_NE(nullptr, icon);
  EXPECT_NE(nullptr, x_icon);
  EXPECT_NE(icon->get_static_value()->value(),
            x_icon->get_static_value()->value());
  auto prickly = find_sfield_named(*drawable_cls, "prickly");
  auto x_prickly = find_sfield_named(*drawable_cls, "x_prickly");
  EXPECT_FIELDS_DIFFERENT(prickly, x_prickly);

  // color
  auto color_cls = find_class_named(classes, "Lcom/facebook/R$color;");
  EXPECT_NE(nullptr, color_cls);
  auto hex_or_file = find_sfield_named(*color_cls, "hex_or_file");
  auto hex_or_file2 = find_sfield_named(*color_cls, "hex_or_file2");
  EXPECT_FIELDS_DIFFERENT(hex_or_file, hex_or_file2);
  auto red = find_sfield_named(*color_cls, "red");
  auto red_duplicate = find_sfield_named(*color_cls, "red_duplicate");
  EXPECT_FIELDS_DIFFERENT(red, red_duplicate);
}

inline void dedupresource_postverify(const DexClasses& classes,
                                     ResourceTableFile* res_table) {
  auto dimen_cls = find_class_named(classes, "Lcom/facebook/R$dimen;");
  EXPECT_NE(nullptr, dimen_cls);
  auto margin_top = find_sfield_named(*dimen_cls, "margin_top");
  auto padding_left = find_sfield_named(*dimen_cls, "padding_left");
  auto padding_right = find_sfield_named(*dimen_cls, "padding_right");
  auto unused_dimen_1 = find_sfield_named(*dimen_cls, "unused_dimen_1");
  auto unused_dimen_2 = find_sfield_named(*dimen_cls, "unused_dimen_2");
  auto welcome_text_size = find_sfield_named(*dimen_cls, "welcome_text_size");
  auto small = find_sfield_named(*dimen_cls, "small");
  auto medium = find_sfield_named(*dimen_cls, "medium");
  auto medium2 = find_sfield_named(*dimen_cls, "medium2");
  auto foo = find_sfield_named(*dimen_cls, "foo");
  auto bar = find_sfield_named(*dimen_cls, "bar");
  auto far = find_sfield_named(*dimen_cls, "far");
  auto baz = find_sfield_named(*dimen_cls, "baz");
  auto boo = find_sfield_named(*dimen_cls, "boo");

  EXPECT_FIELDS_SAME(padding_right, padding_left);
  EXPECT_FIELDS_SAME(padding_right, unused_dimen_2);
  EXPECT_FIELDS_DIFFERENT(padding_right, margin_top);
  EXPECT_FIELDS_DIFFERENT(padding_right, unused_dimen_1);
  EXPECT_FIELDS_DIFFERENT(padding_right, welcome_text_size);

  EXPECT_FIELDS_SAME(medium, medium2);
  EXPECT_FIELDS_SAME(foo, bar);
  EXPECT_FIELDS_DIFFERENT(far, foo);
  EXPECT_FIELDS_DIFFERENT(baz, boo);

  auto margin_top_ids = res_table->get_res_ids_by_name("margin_top");
  auto welcome_text_size_ids =
      res_table->get_res_ids_by_name("welcome_text_size");
  auto padding_left_ids = res_table->get_res_ids_by_name("padding_left");
  auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
  auto unused_dimen_1_ids = res_table->get_res_ids_by_name("unused_dimen_1");
  auto unused_dimen_2_ids = res_table->get_res_ids_by_name("unused_dimen_2");
  auto small_ids = res_table->get_res_ids_by_name("small");
  auto medium_ids = res_table->get_res_ids_by_name("medium");
  auto medium2_ids = res_table->get_res_ids_by_name("medium2");
  auto foo_ids = res_table->get_res_ids_by_name("foo");
  auto bar_ids = res_table->get_res_ids_by_name("bar");
  auto far_ids = res_table->get_res_ids_by_name("far");
  auto baz_ids = res_table->get_res_ids_by_name("baz");
  auto boo_ids = res_table->get_res_ids_by_name("boo");
  EXPECT_EQ(margin_top_ids.size(), 1);
  EXPECT_EQ(welcome_text_size_ids.size(), 1);
  EXPECT_EQ(padding_left_ids.size() + padding_right_ids.size() +
                unused_dimen_2_ids.size(),
            1);
  EXPECT_EQ(unused_dimen_1_ids.size(), 1);
  EXPECT_EQ(small_ids.size(), 1);
  EXPECT_EQ(medium_ids.size() + medium2_ids.size(), 1);
  EXPECT_EQ(foo_ids.size() + bar_ids.size(), 1);
  EXPECT_EQ(far_ids.size(), 1);
  EXPECT_EQ(baz_ids.size(), 1);
  EXPECT_EQ(boo_ids.size(), 1);

  uint32_t equal_id = 0;
  if (padding_left_ids.size() == 1) {
    equal_id = padding_left_ids[0];
  }
  if (padding_right_ids.size() == 1) {
    equal_id = padding_right_ids[0];
  }
  if (unused_dimen_2_ids.size() == 1) {
    equal_id = unused_dimen_2_ids[0];
  }

  uint32_t medium_id = 0;
  if (medium_ids.size() == 1) {
    medium_id = medium_ids[0];
  }
  if (medium2_ids.size() == 1) {
    medium_id = medium2_ids[0];
  }

  uint32_t foo_bar_id = 0;
  if (foo_ids.size() == 1) {
    foo_bar_id = foo_ids[0];
  }
  if (bar_ids.size() == 1) {
    foo_bar_id = bar_ids[0];
  }
  EXPECT_EQ(margin_top_ids[0], margin_top->get_static_value()->value());
  EXPECT_EQ(welcome_text_size_ids[0],
            welcome_text_size->get_static_value()->value());
  EXPECT_EQ(equal_id, padding_left->get_static_value()->value());
  EXPECT_EQ(unused_dimen_1_ids[0], unused_dimen_1->get_static_value()->value());

  EXPECT_EQ(medium_id, medium->get_static_value()->value());
  EXPECT_EQ(medium_id, medium2->get_static_value()->value());

  EXPECT_EQ(foo_bar_id, foo->get_static_value()->value());
  EXPECT_EQ(foo_bar_id, bar->get_static_value()->value());

  auto dup_theme1_ids = res_table->get_res_ids_by_name("DupTheme1");
  auto dup_theme2_ids = res_table->get_res_ids_by_name("DupTheme2");
  EXPECT_EQ(dup_theme1_ids.size() + dup_theme2_ids.size(), 1);
  auto style_cls = find_class_named(classes, "Lcom/facebook/R$style;");
  auto dup_theme1_field = find_sfield_named(*style_cls, "DupTheme1");
  auto dup_theme2_field = find_sfield_named(*style_cls, "DupTheme2");
  EXPECT_FIELDS_SAME(dup_theme1_field, dup_theme2_field);

  auto style_not_sorted_ids = res_table->get_res_ids_by_name("StyleNotSorted");
  auto style_sorted_ids = res_table->get_res_ids_by_name("StyleSorted");
  EXPECT_EQ(style_not_sorted_ids.size() + style_sorted_ids.size(), 1);
  auto style_not_sorted_field = find_sfield_named(*style_cls, "StyleNotSorted");
  auto style_sorted_field = find_sfield_named(*style_cls, "StyleSorted");
  EXPECT_FIELDS_SAME(style_not_sorted_field, style_sorted_field);

  auto theme_different_a_ids =
      res_table->get_res_ids_by_name("ThemeDifferentA");
  auto theme_different_b_ids =
      res_table->get_res_ids_by_name("ThemeDifferentB");
  EXPECT_EQ(theme_different_a_ids.size(), 1);
  EXPECT_EQ(theme_different_b_ids.size(), 1);
  auto theme_different_a_field =
      find_sfield_named(*style_cls, "ThemeDifferentA");
  auto theme_different_b_field =
      find_sfield_named(*style_cls, "ThemeDifferentB");
  EXPECT_FIELDS_DIFFERENT(theme_different_a_field, theme_different_b_field);

  auto same_attribute_a_ids = res_table->get_res_ids_by_name("SameAttributeA");
  auto same_attribute_b_ids = res_table->get_res_ids_by_name("SameAttributeB");
  EXPECT_EQ(same_attribute_a_ids.size() + same_attribute_b_ids.size(), 2);
  auto attr_cls = find_class_named(classes, "Lcom/facebook/R$attr;");
  auto same_attribute_a_field = find_sfield_named(*attr_cls, "SameAttributeA");
  auto same_attribute_b_field = find_sfield_named(*attr_cls, "SameAttributeB");
  EXPECT_FIELDS_DIFFERENT(same_attribute_a_field, same_attribute_b_field);

  // drawable
  auto drawable_cls = find_class_named(classes, "Lcom/facebook/R$drawable;");
  EXPECT_NE(nullptr, drawable_cls);
  auto icon = find_sfield_named(*drawable_cls, "icon");
  auto x_icon = find_sfield_named(*drawable_cls, "x_icon");
  EXPECT_NE(nullptr, icon);
  EXPECT_NE(nullptr, x_icon);
  EXPECT_EQ(icon->get_static_value()->value(),
            x_icon->get_static_value()->value());
  auto prickly = find_sfield_named(*drawable_cls, "prickly");
  auto x_prickly = find_sfield_named(*drawable_cls, "x_prickly");
  EXPECT_FIELDS_SAME(prickly, x_prickly);

  // color
  auto color_cls = find_class_named(classes, "Lcom/facebook/R$color;");
  EXPECT_NE(nullptr, color_cls);
  auto hex_or_file = find_sfield_named(*color_cls, "hex_or_file");
  auto hex_or_file2 = find_sfield_named(*color_cls, "hex_or_file2");
  // Make sure an identical file that is among values in different configs do
  // not accidentially count as duplicates.
  EXPECT_FIELDS_DIFFERENT(hex_or_file, hex_or_file2);
  // Should get dedupped properly
  auto red = find_sfield_named(*color_cls, "red");
  auto red_duplicate = find_sfield_named(*color_cls, "red_duplicate");
  EXPECT_FIELDS_SAME(red, red_duplicate);
}
