/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

TEST_F(ConstantPropagationTest, UnopNegIntFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (neg-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (const v1 -1)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopNegLongFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 9223372036854775807)
      (neg-long v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 9223372036854775807)
      (const-wide v1 -9223372036854775807)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToLongFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -2147483647)
      (int-to-long v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -2147483647)
      (const-wide v1 -2147483647)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToByteFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (int-to-byte v1 v0)

      (const v2 128)
      (int-to-byte v3 v2)

      (const v4 -129)
      (int-to-byte v5 v4)

      (const v6 -2147483648)
      (int-to-byte v7 v6)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (const v1 -1)

      (const v2 128)
      (const v3 -128)

      (const v4 -129)
      (const v5 127)

      (const v6 -2147483648)
      (const v7 0)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToCharFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -65535)
      (int-to-char v1 v0)

      (const v2 2147483647)
      (int-to-char v3 v2)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -65535)
      (const v1 1)

      (const v2 2147483647)
      (const v3 65535)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToShortFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (int-to-short v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (const v1 -1)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopLongToIntFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -9223372036854775807)
      (long-to-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -9223372036854775807)
      (const v1 1)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopFloat) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1078523331) ; float 3.14f
      (neg-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1078523331) ; float 3.14f
      (const v1 -1078523331) ; float -3.14f

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopNegFloatFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1078523331) ; float 3.14f
      (neg-float v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1078523331) ; float 3.14f
      (const v1 -1068960317) ; float -3.14f

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopNegDoubleFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 -4609118966639786721) ; float -3.14f
      (neg-double v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 -4609118966639786721) ; float -3.14f
      (const-wide v1 4614253070214989087)  ; float 3.14f

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopFloatToIntFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1078523331) ; float 3.14f
      (float-to-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1078523331) ; float 3.14f
      (const v1 3)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopFloatToLongFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1089365606) ; float 7.45f
      (float-to-long v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1089365606) ; float 7.45f
      (const-wide v1 7)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToFloatFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (int-to-float v1 v0)

      (const v2 16777216)
      (int-to-float v3 v2)

      (const v4 -2147483648)
      (int-to-float v5 v4)

      (const v6 -16777216)
      (int-to-float v7 v6)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (const v1 1325400064) ; float 2147483647.0 

      (const v2 16777216)
      (const v3 1266679808) ; float 16777216.0  

      (const v4 -2147483648)
      (const v5 -822083584) ; float -2147483648.0

      (const v6 -16777216)
      (const v7 -880803840) ; float -16777216.0 

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopLongToFloatFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 -9223372036854775808)
      (long-to-float v1 v0)

      (const-wide v2 -9223372036854775807)
      (long-to-float v3 v2)

      (const-wide v4 9223372036854775807)
      (long-to-float v5 v4)

      (const-wide v6 -4294967296)
      (long-to-float v7 v6)

      (const-wide v8 4294967296)
      (long-to-float v9 v8)

      (const-wide v10 -2147483649)
      (long-to-float v11 v10)

      (const-wide v12 2147483648)
      (long-to-float v13 v12)

      (const-wide v14 -2147483648)
      (long-to-float v15 v14)

      (const-wide v16 -2147483647)
      (long-to-float v17 v16)

      (const-wide v18 2147483647)
      (long-to-float v19 v18)

      (const-wide v20 -51)
      (long-to-float v21 v20)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 -9223372036854775808)
      (const v1 -553648128) ; float -9223372036854775808f

      (const-wide v2 -9223372036854775807)
      (const v3 -553648128) ; float -9223372036854775808f

      (const-wide v4 9223372036854775807)
      (const v5 1593835520) ; float 9223372036854775808f

      (const-wide v6 -4294967296)
      (const v7 -813694976) ; float -4294967296f

      (const-wide v8 4294967296)
      (const v9 1333788672) ; float 4294967296f

      (const-wide v10 -2147483649)
      (const v11 -822083584) ; float -2147483648f

      (const-wide v12 2147483648)
      (const v13 1325400064) ; float 2147483648f

      (const-wide v14 -2147483648)
      (const v15 -822083584) ; float -2147483648f 

      (const-wide v16 -2147483647)
      (const v17 -822083584) ; float -2147483648f

      (const-wide v18 2147483647)
      (const v19 1325400064) ; float 2147483648f

      (const-wide v20 -51)
      (const v21 -1035206656) ; float -51f 

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopDoubleToIntFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 4638375897931557372) ; float 123.286f
      (double-to-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 4638375897931557372) ; float 123.286f
      (const v1 123)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopDoubleToLongFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 4638375897931557372) ; float 123.286f
      (double-to-long v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 4638375897931557372) ; float 123.286f 
      (const-wide v1 123)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToDoubleFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -2147483648)
      (int-to-double v1 v0)

      (const v2 16777216)
      (int-to-double v3 v2)

      (const v4 -16777216)
      (int-to-double v5 v4)

      (const v6 2147483647)
      (int-to-double v7 v6)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -2147483648)
      (const-wide v1 -4476578029606273024) ; double -2147483648f

      (const v2 16777216)
      (const-wide v3 4715268809856909312) ; double 16777216f 

      (const v4 -16777216)
      (const-wide v5 -4508103226997866496) ; double -16777216f

      (const v6 2147483647)
      (const-wide v7 4746794007244308480) ; double  2147483647f
      
      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopLongToDoubleFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 -9223372036854775808)
      (long-to-double v1 v0)

      (const-wide v2 -9223372036854775807)
      (long-to-double v3 v2)

      (const-wide v4 9223372036854775807)
      (long-to-double v5 v4)

      (const-wide v6 -140739635871745)
      (long-to-double v7 v6)

      (const-wide v8 140739635871745)
      (long-to-double v9 v8)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 -9223372036854775808)
      (const-wide v1 -4332462841530417152) ; double -9223372036854775808f 

      (const-wide v2 -9223372036854775807)
      (const-wide v3 -4332462841530417152) ; double -9223372036854775808f

      (const-wide v4 9223372036854775807)
      (const-wide v5 4890909195324358656) ; double 9223372036854775808f

      (const-wide v6 -140739635871745)
      (const-wide v7 -4404520366847819744) ; double -140739635871745f

      (const-wide v8 140739635871745)
      (const-wide v9 4818851670006956064) ; double 140739635871745f

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopDoubleToFloatFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 4607182418800017408) ; float 1.0000000000000001f
      (double-to-float v1 v0)

      (const-wide v2 -4620693219483568747) ; float -0.4999999f
      (double-to-float v3 v2)

      (const-wide v4 -4620693217682128896) ; float -0.5f 
      (double-to-float v5 v4)

      (const-wide v6 4631135798580605878) ; float 42.199f
      (double-to-float v7 v6)

      (const-wide v8 -4592236238274169930) ; float -42.199f
      (double-to-float v9 v8)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (

      (const-wide v0 4607182418800017408) ; double 1.0000000000000001f
      (const v1 1065353216) ; float 1.0000000000000001f 

      (const-wide v2 -4620693219483568747) ; double -0.4999999f
      (const v3 -1090519043) ; float -0.4999999f 

      (const-wide v4 -4620693217682128896) ; double -0.5f
      (const v5 -1090519040) ; float -0.5f

      (const-wide v6 4631135798580605878)  ; double 42.199f
      (const v7 1109969863) ; float 42.199f

      (const-wide v8 -4592236238274169930) ; double -42.199f
      (const v9 -1037513785) ; float -42.199f 


      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopFloatToDoubleFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v4 -1037513785) ; float -42.199f
      (float-to-double v5 v4)

      (const v6 1109969863) ; float 42.199f
      (float-to-double v7 v6)

      (const v8 -1090519043) ; float -0.4999999f
      (float-to-double v9 v8)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v4 -1037513785) ; float -42.199f
      (const-wide v5 -4592236238089486336) ; double -42.199f

      (const v6 1109969863)  ; float 42.199f
      (const-wide v7 4631135798765289472) ; double 42.199f

      (const v8 -1090519043) ; float -0.4999999f
      (const-wide v9 -4620693219292741632) ; double -0.4999999f
      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
