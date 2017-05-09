/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Resolver.h"
#include "Show.h"
#include "VerifyUtil.h"

TEST_F(PostVerify, InterfaceRemoval) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  ASSERT_NE(cls_a, nullptr);
  auto itfs = cls_a->get_interfaces()->get_type_list();
  ASSERT_TRUE(itfs.empty());
}

TEST_F(PostVerify, ParserRemoval) {
  auto cls_model = find_class_named(
    classes, "Lcom/facebook/redextest/EnclosingModels$AModel;");
  ASSERT_NE(cls_model, nullptr);
  auto dmethods = cls_model->get_dmethods();
  ASSERT_TRUE(dmethods.size() == 2);
  auto cls_parser = find_class_named(
    classes, "Lcom/facebook/redextest/EnclosingParsers$AParser;");
  ASSERT_NE(cls_parser, nullptr);
  dmethods = cls_parser->get_dmethods();
  ASSERT_TRUE(dmethods.size() == 1);

  cls_model = find_class_named(
    classes, "Lcom/facebook/redextest/EnclosingModels$BModel;");
  ASSERT_NE(cls_model, nullptr);
  dmethods = cls_model->get_dmethods();
  ASSERT_TRUE(dmethods.size() == 3);
  cls_parser = find_class_named(
    classes, "Lcom/facebook/redextest/EnclosingParsers$BParser;");
  ASSERT_NE(cls_parser, nullptr);
  dmethods = cls_parser->get_dmethods();
  ASSERT_TRUE(dmethods.size() == 1);
}
