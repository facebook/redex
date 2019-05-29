/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <iterator>

#include "ControlFlow.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IntroduceSwitch.h"

struct InsertSwitchTest : testing::Test {
  DexMethod* m_method;

  InsertSwitchTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(get_void_type(), args);
    m_method = static_cast<DexMethod*>(DexMethod::make_method(
        get_object_type(), DexString::make_string("testMethod"), proto));
    m_method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  }

  ~InsertSwitchTest() { delete g_redex; }
};

// Code:    if r == i then A else if r == i+1 then B else if r == i+2 then C; D
// Result:  switch r {ABC}; D
TEST_F(InsertSwitchTest, simpleCompactSwitch) {
  using namespace dex_asm;

  auto code = assembler::ircode_from_string(R"(
    (
      (const v4 2)

      ; let's have an infinite loop so that the last block has a succ
      (:begin)
      (const v0 0)
      (if-ne v0 v3 :a)

      (add-int v0 v1 v1)
      (goto :d)

      (:a)
      (const v0 1)
      (if-ne v0 v3 :b)

      (add-int v0 v2 v2)
      (goto :d)

      (:b)
      (const v0 2)
      (if-ne v0 v3 :c)

      (add-int v0 v4 v4)
      (goto :d)

      (:c)
      (nop)

      (:d)
      (add-int v4 v4 v4)
      (goto :begin)
    )
  )");
  m_method->set_code(std::move(code));

  IntroduceSwitchPass().run(m_method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v4 2)

      (:begin)
      (const v0 0)
      (switch v3 (:a :b :c))

      (nop)

      (:end)
      (add-int v4 v4 v4)
      (goto :begin)

      (:c 2)
      (add-int v0 v4 v4)
      (goto :end)

      (:b 1)
      (add-int v0 v2 v2)
      (goto :end)

      (:a 0)
      (add-int v0 v1 v1)
      (goto :end)
    )
  )");

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(m_method->get_code()));
}

// Code:    if r==i A else if r==i+10 B else if r==i+2 C
// Result:  switch r {ABC}
TEST_F(InsertSwitchTest, simplifySparseSwitch) {
  using namespace dex_asm;

  auto code = assembler::ircode_from_string(R"(
    (
      (const v4 2)

      (:begin)
      (const v0 0)
      (if-ne v0 v3 :a)

      (add-int v1 v1 v1)
      (goto :exit)

      (:a)
      (const v0 10)
      (if-ne v0 v3 :b)

      (add-int v2 v2 v2)
      (goto :exit)

      (:b)
      (const v0 2)
      (if-ne v0 v3 :c)

      (add-int v1 v2 v1)
      (goto :exit)

      (:c)
      (nop)

      (:exit)
      (add-int v4 v1 v2)
      (goto :begin)
    )
  )");
  m_method->set_code(std::move(code));

  IntroduceSwitchPass().run(m_method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v4 2)

      (:begin)
      (const v0 0)
      (switch v3 (:a :b :c))

      (nop)

      (:exit)
      (add-int v4 v1 v2)
      (goto :begin)

      (:c 10)
      (add-int v2 v2 v2)
      (goto :exit)

      (:b 2)
      (add-int v1 v2 v1)
      (goto :exit)

      (:a 0)
      (add-int v1 v1 v1)
      (goto :exit)
    )
  )");

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(m_method->get_code()));
}

// Code:    if r==i A else if r==i+10 B
// Result:  no change
TEST_F(InsertSwitchTest, skipSmallChain) {
  using namespace dex_asm;

  auto code = assembler::ircode_from_string(R"(
    (
      (const v3 2)

      (:begin)
      (const v0 0)
      (if-ne v0 v3 :a)

      (add-int v0 v0 v0)

      (:exit)
      (add-int v0 v0 v0)
      (goto :begin)

      (:a)
      (const v0 10)
      (if-ne v0 v3 :b)

      (add-int v0 v0 v1)
      (goto :exit)

      (:b)
      (nop)
      (goto :exit)
    )
  )");
  m_method->set_code(std::move(code));
  const std::string& input = assembler::to_string(m_method->get_code());

  IntroduceSwitchPass().run(m_method);

  EXPECT_EQ(input, assembler::to_string(m_method->get_code()));
}
