/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <unordered_map>
#include <utility>

#include <gtest/gtest.h>

#include "MethodClosures.h"
#include "MethodSplitter.h"

#include "AddCheckCast.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

class AddCheckCastTest : public RedexTest {
 public:
  static std::pair<DexClass*, DexMethod*> create_class(
      const std::string& name,
      const std::string& sig,
      const std::string& code_str) {
    // Create a totally new class.
    ClassCreator cc{DexType::make_type(name.c_str())};
    cc.set_super(type::java_lang_Object());

    auto ircode = assembler::ircode_from_string(code_str);
    ircode->build_cfg();
    auto m =
        DexMethod::make_method(name + ".bar:" + sig)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, std::move(ircode), false);
    m->set_deobfuscated_name(show(m));
    cc.add_method(m);
    return {cc.create(), m};
  }

  static DexClass* create_empty_class(const std::string& name) {
    ClassCreator cc{DexType::make_type(name.c_str())};
    cc.set_super(type::java_lang_Object());
    return cc.create();
  }
  static DexStore* create_store(std::string store_name,
                                const std::vector<DexClass*>& classes) {
    DexStore* store = new DexStore(std::move(store_name));
    store->get_dexen().push_back(classes);
    return store;
  }

  static std::pair<DexStore*, DexMethod*> create_test_root_store(
      std::string& test_method_sig, std::string& test_method_code_str) {
    auto [cls, m] =
        create_class("LFoo;", test_method_sig, test_method_code_str);
    return {create_store("classes", {cls}), m};
  }

  static DexStore* create_test_non_root_store(
      std::string& problematic_type_name) {
    auto cls = create_empty_class(problematic_type_name);
    return create_store("longtail", {cls});
  }

  void test(std::string problematic_type_name,
            std::string sig,
            std::string code_str,
            const std::string& expected) {
    DexStoresVector stores;
    auto [root_store, method] = create_test_root_store(sig, code_str);
    stores.emplace_back(*root_store);
    auto non_root_store = create_test_non_root_store(problematic_type_name);
    stores.emplace_back(*non_root_store);
    AddCheckCastPass pass;
    XStoreRefs xstores(stores);

    pass.run_on_method(method, &xstores);

    auto expected_ircode = assembler::ircode_from_string(expected);
    expected_ircode->build_cfg();

    auto result_ir_list = method->get_code()->cfg().linearize();
    auto result_ir_it = result_ir_list->begin();

    auto expected_ir_list = expected_ircode->cfg().linearize();
    auto expected_ir_it = expected_ir_list->begin();

    while (result_ir_it != result_ir_list->end() &&
           expected_ir_it != expected_ir_list->end()) {
      if (result_ir_it->insn->opcode() == OPCODE_NOP) {
        EXPECT_TRUE(expected_ir_it->insn->opcode() == OPCODE_NOP);
        ++result_ir_it;
        ++expected_ir_it;
        continue;
      }
      auto eq = *result_ir_it == *expected_ir_it;
      if (!eq) {
        std::cout << show(*(result_ir_it)) << "|" << show(*(expected_ir_it))
                  << std::endl;
      }
      EXPECT_TRUE(eq);
      ++result_ir_it;
      ++expected_ir_it;
    }

    EXPECT_EQ(result_ir_it, result_ir_list->end());
    EXPECT_EQ(expected_ir_it, expected_ir_list->end());

    EXPECT_TRUE(
        method->get_code()->cfg().structural_equals(expected_ircode->cfg()));
  }
};
/*
new instructions template
      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v2)
      (move-object v0 v2)
*/
TEST_F(AddCheckCastTest, testLoadParam) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LBar;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v0)
      (return v0)
    ))";
  std::string after = R"(
    (
      (load-param-object v0)
      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v1)
      (move-object v0 v1)
      (return v0)
    ))";

  test(problematic_type_name, sig, before, after);
}

TEST_F(AddCheckCastTest, testMoveObject) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LBar;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v1)
      (move-object v0 v1)
      (return v0)
    ))";
  std::string after = R"(
    (
      (load-param-object v1)
      (move-object v0 v1)
      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v2)
      (move-object v0 v2)
      (return v0)
    ))";

  test(problematic_type_name, sig, before, after);
}

TEST_F(AddCheckCastTest, testMoveException) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LBar;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v1)

      (.try_start t_0)
      (sget-object "LA;.f1:LBar;")
      (.try_end t_0)

      (:block_catch_t_0)
      (.catch (t_0) "LBar;")
      (sget-object "LA;.f1:LBar;")
      (move-exception v0)

      (return v0)
    ))";
  std::string after = R"(
    (
      (load-param-object v1)

      (.try_start t_0)
      (sget-object "LA;.f1:LBar;")
      (.try_end t_0)

      (:block_catch_t_0)
      (.catch (t_0) "LBar;")
      (sget-object "LA;.f1:LBar;")
      (move-exception v0)

      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v2)
      (move-object v0 v2)
      (return v0)
    ))";

  test(problematic_type_name, sig, before, after);
}

TEST_F(AddCheckCastTest, testAget) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LBar;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v0)
      (new-array v1 "LBar;")
      (aget-object v0 v1)
      (return v0)
    ))";
  std::string after = R"(
    (
      (load-param-object v0)
      (new-array v1 "LBar;")
      (aget-object v0 v1)
      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v1)
      (move-object v0 v1)
      (return v0)
    ))";

  test(problematic_type_name, sig, before, after);
}

TEST_F(AddCheckCastTest, testIgetObject) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LBar;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v0)
      (iget-object v0 "LA;.f1:LBar;")
      (move-result-pseudo-object v0)
      (return v0)
    ))";
  std::string after = R"(
    (
      (load-param-object v0)
      (iget-object v0 "LA;.f1:LBar;")
      (move-result-pseudo-object v0)
      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v1)
      (move-object v0 v1)
      (return v0)
    ))";

  test(problematic_type_name, sig, before, after);
}

TEST_F(AddCheckCastTest, testSgetObject) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LBar;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v0)
      (sget-object "LA;.f1:LBar;")
      (move-result-pseudo-object v0)
      (return v0)
    ))";
  std::string after = R"(
    (
      (load-param-object v0)
      (sget-object "LA;.f1:LBar;")
      (move-result-pseudo-object v0)
      (check-cast v0 "LFoo;")
      (move-result-pseudo-object v1)
      (move-object v0 v1)
      (return v0)
    ))";

  test(problematic_type_name, sig, before, after);
}

TEST_F(AddCheckCastTest, testReturnInTry) {
  std::string problematic_type_name = "LBar;";
  std::string sig = "(LTest;)LFoo;";
  std::string before = R"(
    (
      (load-param-object v1)

      (.try_start t_0)
      check-cast v1 "LFoo;"
      (.try_end t_0)

      (:block_catch_t_0)
      (.catch (t_0) "LBar;")
      check-cast v1 "LFoo;"
      (move-result-pseudo-object v2)
      (move-exception v0)

      (.try_start t_1)
      check-cast v1 "LFoo;"
      (return v0)
      (.try_end t_1)
      (:block_catch_t_1)
      (.catch (t_1) "LTest;")
      check-cast v1 "LFoo;"
      (move-result-pseudo-object v2)
      (move-exception v0)

      (return v2)
    ))";

  test(problematic_type_name, sig, before, before);
}
