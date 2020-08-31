/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "AnnoUtils.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "RemoveEmptyClasses.h"
#include "Show.h"

/*
 * This test verifies if default annotation values can be parsed correctly
 */

class DefaultAnnotationTest : public RedexIntegrationTest {};

TEST_F(DefaultAnnotationTest, defaultAnnotation) {
  TRACE(ANNO, 9, "Loaded classes: %d \n", classes->size());
  for (const auto& dex_class : *classes) {
    TRACE(ANNO, 9, "Class %s\n", SHOW(dex_class));
    TRACE(ANNO, 9, "%s\n", SHOW(dex_class->get_anno_set()));
    for (const auto& dex_method : dex_class->get_dmethods()) {
      TRACE(ANNO, 9, "method %s asset %p \n", dex_method->c_str(),
            dex_method->get_anno_set());
      TRACE(ANNO, 9, "%s\n", show(dex_method->get_anno_set()).c_str());

      DexAnnotationSet* set = dex_method->get_anno_set();
      std::string int_anno_name = "intVal";
      std::string str_anno_name = "strVal";
      std::string bool_anno_name = "booleanVal";
      std::string no_such_anno_name = "noSuchVal";

      if (set) {
        for (auto const anno : set->get_annotations()) {
          // annotation with default value
          if ("foo" == dex_method->str()) {
            // ensure that method foo() has only
            // defaults in the test Java source
            EXPECT_EQ(anno->anno_elems().size(), 0);

            int intResult =
                parse_int_anno_value(dex_method, anno->type(), int_anno_name);
            TRACE(ANNO, 9, "default value for %s is  %d\n",
                  int_anno_name.c_str(), intResult);
            EXPECT_EQ(42, intResult);

            const std::string strResult =
                parse_str_anno_value(dex_method, anno->type(), str_anno_name);
            TRACE(ANNO, 9, "default value for %s is  %s\n",
                  str_anno_name.c_str(), strResult.c_str());
            EXPECT_STREQ("defaultStrValue", strResult.c_str());

            bool boolResult =
                parse_bool_anno_value(dex_method, anno->type(), bool_anno_name);
            TRACE(ANNO, 9, "default value for %s is  %s\n",
                  bool_anno_name.c_str(), boolResult ? "true" : "false");
            EXPECT_EQ(1, (int)boolResult);

            const DexEncodedValue* result =
                parse_default_anno_value(anno->type(), no_such_anno_name);
            TRACE(ANNO, 9, "default value for %s is %p\n",
                  no_such_anno_name.c_str(), result);
            EXPECT_EQ(NULL, result);
          }

          if ("bar" == dex_method->str()) {
            // ensure that method bar() has
            // 3 annotations in the test Java source
            EXPECT_EQ(anno->anno_elems().size(), 3);

            int intResult =
                parse_int_anno_value(dex_method, anno->type(), int_anno_name);
            TRACE(ANNO, 9, "value for %s is %d\n", int_anno_name.c_str(),
                  intResult);
            EXPECT_EQ(100, intResult);

            const std::string strResult =
                parse_str_anno_value(dex_method, anno->type(), str_anno_name);
            TRACE(ANNO, 9, "value for %s is %s\n", str_anno_name.c_str(),
                  strResult.c_str());
            EXPECT_STREQ("overriddenStrValue", strResult.c_str());

            bool boolResult =
                parse_bool_anno_value(dex_method, anno->type(), bool_anno_name);
            TRACE(ANNO, 9, "value for %s is %s\n", bool_anno_name.c_str(),
                  boolResult ? "true" : "false");
            EXPECT_EQ(0, (int)boolResult);

            const DexEncodedValue* result =
                parse_default_anno_value(anno->type(), no_such_anno_name);
            TRACE(ANNO, 9, "value for %s is %p\n", no_such_anno_name.c_str(),
                  result);
            EXPECT_EQ(NULL, result);
          }
        }
      }
    }
  }
}
