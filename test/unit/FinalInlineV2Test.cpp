/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "FinalInlineV2.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

struct FinalInlineTest : public RedexTest {};

TEST_F(FinalInlineTest, encodeValues) {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(get_object_type());
  auto field_bar = static_cast<DexField*>(DexField::make_field("LFoo;.bar:I"));
  field_bar->make_concrete(ACC_PUBLIC | ACC_STATIC,
                           DexEncodedValue::zero_for_type(get_int_type()));
  cc.add_field(field_bar);
  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (return-void)
     )
    )
  )"));
  auto cls = cc.create();

  FinalInlinePassV2::run({cls});

  EXPECT_EQ(cls->get_clinit(), nullptr);
  EXPECT_EQ(field_bar->get_static_value()->value(), 1);
}
