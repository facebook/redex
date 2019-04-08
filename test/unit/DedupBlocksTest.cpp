/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <iterator>

#include "Creators.h"
#include "ControlFlow.h"
#include "DedupBlocksPass.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"

struct Branch {
  MethodItemEntry* source;
  MethodItemEntry* target;
};

void run_passes(std::vector<Pass*> passes, std::vector<DexClass*> classes) {
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore store(dm);
  store.add_classes(classes);
  stores.emplace_back(std::move(store));
  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_config(conf_obj);
  manager.run_passes(stores, dummy_config);
}

struct DedupBlocksTest : testing::Test {
  DexClass* m_class;
  DexTypeList* m_args;
  DexProto* m_proto;
  DexType* m_type;
  ClassCreator* m_creator;

  DedupBlocksTest() {
    g_redex = new RedexContext();
    m_args = DexTypeList::make_type_list({});
    m_proto = DexProto::make_proto(get_void_type(), m_args);
    m_type = DexType::make_type("testClass");

    m_creator = new ClassCreator(m_type);
    m_class = m_creator->get_class();
  }

  DexMethod* get_fresh_method(const std::string& name) {
    DexMethod* method = static_cast<DexMethod*>(DexMethod::make_method(
        m_type, DexString::make_string(name), m_proto));
    method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(std::make_unique<IRCode>(method, 1));
    m_creator->add_method(method);
    return method;
  }

  void run_dedup_blocks() {
    std::vector<Pass*> passes = {
      new DedupBlocksPass()
    };
    std::vector<DexClass*> classes = {
      m_class
    };
    run_passes(passes, classes);
  }

  ~DedupBlocksTest() { delete g_redex; }
};

// in Code:     A B E C D          (where C == D)
// in CFG:      A -> B -> C -> E
//               \            /
//                >  --   D  >
//
// out Code:    A B E C
// out CFG:     A -> B -> C -> E
//               \       /
//                > --- >
TEST_F(DedupBlocksTest, simplestCase) {
  using namespace dex_asm;
  DexMethod* method = get_fresh_method("simplestCase");

  auto str = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      ; B
      (mul-int v0 v0 v0)
      (goto :C)

      (:E)
      (return-void)

      (:C)
      (add-int v0 v0 v0)
      (goto :E)

      (:D)
      (add-int v0 v0 v0)
      (goto :E)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_str = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :C)

      ; B
      (mul-int v0 v0 v0)

      (:C)
      (add-int v0 v0 v0)

      ; E
      (return-void)

      ; no D!
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(method->get_code()));
}

// in Code:     A B E C D          (where C and D ends with same instructions)
// in CFG:      A -> B -> C -> E
//               \            /
//                >  --   D  >
//
// out Code:    A B E C
// out CFG:     A -> B -> C' -> F -> E
//               \             /
//                > --------- D'
TEST_F(DedupBlocksTest, simplestPostfixCase) {
  using namespace dex_asm;
  DexMethod* method = get_fresh_method("simplestPostfixCase");

  auto str = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      ; B
      (mul-int v0 v0 v0)
      (goto :C)

      (:E)
      (return-void)

      (:C)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (goto :E)

      (:D)
      (const v1 1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (goto :E)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_str = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      ; B
      (mul-int v0 v0 v0)

      ; C
      (mul-int v0 v0 v0)

      (:F)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)

      (:E)
      (return-void)

      (:D)
      (const v1 1)
      (goto :F)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(method->get_code()));
}

TEST_F(DedupBlocksTest, postfixDiscardingOneCase) {
  using namespace dex_asm;
  DexMethod* method = get_fresh_method("postfixDiscardingOneCase");

  auto str = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      ; B
      (mul-int v0 v0 v0)
      (goto :C)

      (:E)
      (return-void)

      (:C)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (goto :E)

      (:D)
      (if-eqz v0 :F)
      (goto :G)

      (:F)
      (const v2 2)
      (goto :E)

      (:G)
      (const v1 1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (goto :E)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_str = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      ; B
      (mul-int v0 v0 v0)

      (:C)
      (mul-int v0 v0 v0)

      (:H)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)

      (:E)
      (return-void)

      (:D)
      (if-eqz v0 :F)

      (:G)
      (const v1 1)
      (goto :H)

      (:F)
      (const v2 2)
      (goto :E)

    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(method->get_code()));
}

TEST_F(DedupBlocksTest, deepestIsNotTheBestCase) {
  using namespace dex_asm;
  DexMethod* method = get_fresh_method("deepestIsNotTheBestCase");

  auto str = R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c :d :e :f))
      (return v0)

      (:a 0)
      (return v0)

      (:b 1)
      (const v1 1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)

      (:c 2)
      (const v1 2)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)

      (:d 3)
      (const v0 0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)

      (:e 4)
      (const v0 0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)

      (:f 5)
      (const v0 0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c :d :e :f))

      (:a 0)
      (return v0)

      (:f 5)
      (const v0 0)
      (goto :g)

      (:e 4)
      (const v0 0)
      (goto :g)

      (:d 3)
      (const v0 0)
      (goto :g)

      (:c 2)
      (const v1 2)
      (goto :g)

      (:b 1)
      (const v1 1)

      (:g)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)
    )
  )";

  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(method->get_code()));
}

TEST_F(DedupBlocksTest, postfixSwitchCase) {
  using namespace dex_asm;
  DexMethod* method = get_fresh_method("postfixSwitchCase");

  auto str = R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c))

      (:a 0)
      (return v0)

      (:b 1)
      (const v1 1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)

      (:c 2)
      (const v0 0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c))

      (:a 0)
      (return v0)

      (:c 2)
      (const v0 0)
      (goto :d)

      (:b 1)
      (const v1 1)

      (:d)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v1)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(method->get_code()));
}

TEST_F(DedupBlocksTest, noDups) {
  auto str = R"(
    (
      (const v0 0)
      (if-eqz v0 :lbl)

      (const v0 1)

      (:lbl)
      (return v0)
    )
  )";

  auto method = get_fresh_method("noDups");
  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(str);

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(method->get_code()));
}

TEST_F(DedupBlocksTest, repeatedSwitchBlocks) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c))
      (return v0)

      (:a 0)
      (return v0)

      (:b 1)
      (return v1)

      (:c 2)
      (return v1)
    )
  )");

  auto method = get_fresh_method("repeatedSwitchBlocks");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c))

      (:a 0)
      (return v0)

      (:c 2)
      (:b 1)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code));
}

TEST_F(DedupBlocksTest, diffSuccessorsNoChange1) {
  auto str = R"(
    (
      (const v0 0)
      (if-eqz v0 :left)

      ; right
      ; same code as `:left` block but different successors
      (const v1 1)
      (if-eqz v1 :right2)

      (:middle)
      (return-void)

      (:right2)
      (const v3 3)
      (goto :middle)

      (:left)
      (const v1 1)
      (if-eqz v1 :left2)
      (goto :middle)

      (:left2)
      (const v2 2)
      (goto :middle)
    )
  )";

  auto input_code = assembler::ircode_from_string(str);
  auto method = get_fresh_method("diffSuccessorsNoChange1");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(str);

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code));
}

TEST_F(DedupBlocksTest, diffSuccessorsNoChange2) {
  auto str = R"(
    (
      (const v0 0)
      (if-eqz v0 :left)

      ; right
      ; same code as `:left` block but different successors
      (const v1 1)
      (if-eqz v1 :middle)

      ; right2
      (const v3 3)

      (:middle)
      (return-void)

      (:left)
      (const v1 1)
      (if-eqz v1 :middle)

      ; left2
      (const v2 2)
      (goto :middle)
    )
  )";

  auto input_code = assembler::ircode_from_string(str);
  auto method = get_fresh_method("diffSuccessorsNoChange2");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(str);

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code));
}

TEST_F(DedupBlocksTest, diamond) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :left)
      (goto :right)

      (:left)
      (const v1 1)
      (goto :middle)

      (:right)
      (const v1 1)

      (:middle)
      (return-void)
    )
  )");

  auto method = get_fresh_method("diamond");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :left)

      (:left)
      (const v1 1)

      (:middle)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code));
}

// in Code:  A B C (where B == C,
//      and they contain a pair of new-instance and constructor instructions)
// in CFG:   A -> B
//            \
//             > C
// out Code: A B
// out CFG:  A -> B
TEST_F(DedupBlocksTest, blockWithNewInstanceAndConstroctor) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (:a)
      (const v0 0)
      (const v1 1)
      (if-eqz v0 :c)

      (:b)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "testClass.<init>:(I)V")
      (throw v0)

      (:c)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "testClass.<init>:(I)V")
      (throw v0)
    )
  )");
  auto method = get_fresh_method("blockWithNewInstanceAndConstroctor");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (:a)
      (const v0 0)
      (const v1 1)
      (if-eqz v0 :c)

      (:b)
      (:c)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "testClass.<init>:(I)V")
      (throw v0)
    )
  )");

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code));
}

// in Code: A B C D E (where C == E,
//      and they construct an object from B and D respectively)
// in CFG:  A -> B -> C
//           \
//            > D -> E
// out Code: the same as the in Code
// out CFG: the same as the in CFG
TEST_F(DedupBlocksTest, constructsObjectFromAnotherBlock) {
  std::string str_code = R"(
    (
      (:a)
      (const v0 0)
      (if-eqz v0 :d)

      (:b)
      (new-instance "testClass")
      (move-result-pseudo-object v0)

      (:c)
      (const v1 1)
      (invoke-direct (v0 v1) "testClass.<init>:(I)V")
      (throw v0)

      (:d)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (const v1 2)

      (:e)
      (const v1 1)
      (invoke-direct (v0 v1) "testClass.<init>:(I)V")
      (throw v0)
    )
  )";
  auto input_code = assembler::ircode_from_string(str_code);
  auto method = get_fresh_method("constructsObjectFromAnotherBlock");
  method->set_code(std::move(input_code));
  auto code = method->get_code();
  run_dedup_blocks();
  auto expect_code = assembler::ircode_from_string(str_code);
  EXPECT_EQ(assembler::to_string(expect_code.get()),
            assembler::to_string(code));
}

TEST_F(DedupBlocksTest, dedupCatchBlocks) {
  std::string str_code = R"(
    (
      (.try_start t_0)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "testClass.<init>:()V")
      (.try_end t_0)

      (.try_start t_2)
      (iget v0 "testClass;.a:I")
      (move-result-pseudo v2)
      (.try_end t_2)

      (.try_start t_1)
      (iget v0 "testClass;.b:I")
      (move-result-pseudo v3)
      (.try_end t_1)

      (return-void)

      (:block_catch_t_0)
      (.catch (t_0))
      (move-exception v2)
      (throw v2)

      (:block_catch_t_1)
      (.catch (t_1))
      (move-exception v2)
      (throw v2)

      (:block_catch_t_2)
      (.catch (t_2))
      (throw v0)
    )
  )";
  auto input_code = assembler::ircode_from_string(str_code);
  auto method = get_fresh_method("dedupCatchBlocks");
  method->set_code(std::move(input_code));
  auto code = method->get_code();
  run_dedup_blocks();

  std::string expect_str = R"(
    (
      (.try_start t_0)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "testClass.<init>:()V")
      (.try_end t_0)

      (.try_start t_2)
      (iget v0 "testClass;.a:I")
      (move-result-pseudo v2)
      (.try_end t_2)

      (.try_start t_0)
      (iget v0 "testClass;.b:I")
      (move-result-pseudo v3)
      (.try_end t_0)

      (return-void)

      (:block_catch_t_0)
      (.catch (t_0))
      (move-exception v2)
      (throw v2)

      (:block_catch_t_2)
      (.catch (t_2))
      (throw v0)
    )
  )";
  auto expect_code = assembler::ircode_from_string(expect_str);
  expect_code->build_cfg(true);
  expect_code->clear_cfg();

  EXPECT_EQ(assembler::to_string(expect_code.get()),
            assembler::to_string(code));
}

TEST_F(DedupBlocksTest, dontDedupCatchBlockAndNonCatchBlock) {
  std::string str_code = R"(
    (
      (.try_start t_0)
      (new-instance "testClass")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "testClass.<init>:()V")
      (.try_end t_0)

      (if-eqz v0 :block_no_catch)
      (return-void)

      (:block_catch_t_0)
      (.catch (t_0))
      (move-exception v2)
      (throw v2)

      (:block_no_catch)
      (move-exception v2)
      (throw v2)
    )
  )";
  auto input_code = assembler::ircode_from_string(str_code);
  auto method = get_fresh_method("dontDedupCatchBlockAndNonCatchBlock");
  method->set_code(std::move(input_code));
  auto code = method->get_code();
  run_dedup_blocks();

  auto expect_code = assembler::ircode_from_string(str_code);
  expect_code->build_cfg(true);
  expect_code->clear_cfg();

  EXPECT_EQ(assembler::to_string(expect_code.get()),
            assembler::to_string(code));
}
