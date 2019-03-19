/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApiLevelChecker.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Inliner.h"
#include "RedexTest.h"

struct MethodInlineTest : public RedexTest {};

void test_inliner(const std::string& caller_str,
                  const std::string& callee_str,
                  const std::string& expected_str) {
  auto caller = assembler::ircode_from_string(caller_str);
  auto callee = assembler::ircode_from_string(callee_str);

  const auto& callsite = std::find_if(
      caller->begin(), caller->end(), [](const MethodItemEntry& mie) {
        return mie.type == MFLOW_OPCODE && is_invoke(mie.insn->opcode());
      });
  inliner::inline_method(caller.get(), callee.get(), callsite);

  auto expected = assembler::ircode_from_string(expected_str);

  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(caller.get()));
}

DexClass* create_a_class(const char* description) {
  ClassCreator cc(DexType::make_type(description));
  cc.set_super(get_object_type());
  return cc.create();
}

/**
 * Create a method like
 * void {{name}}() {
 *   const v0 {{val}};
 * }
 */
DexMethod* make_a_method(DexClass* cls, const char* name, int val) {
  auto proto =
      DexProto::make_proto(get_void_type(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(
      cls->get_type(), DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_PUBLIC);
  auto main_block = mc.get_main_block();
  auto loc = mc.make_local(get_int_type());
  main_block->load_const(loc, val);
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  return method;
}

/**
 * Create a method calls other methods.
 * void {{name}}() {
 *   other1();
 *   other2();
 *   ...
 * }
 */
DexMethod* make_a_method_calls_others(DexClass* cls,
                                      const char* name,
                                      std::vector<DexMethod*> methods) {
  auto proto =
      DexProto::make_proto(get_void_type(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(
      cls->get_type(), DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_PUBLIC);
  auto main_block = mc.get_main_block();
  for (auto callee : methods) {
    main_block->invoke(callee, {});
  }
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  return method;
}

/*
 * Test that we correctly insert move instructions that map caller args to
 * callee params.
 */
TEST_F(MethodInlineTest, insertMoves) {
  using namespace dex_asm;
  auto callee = static_cast<DexMethod*>(DexMethod::make_method(
      "Lfoo;", "testCallee", "V", {"I", "Ljava/lang/Object;"}));
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  callee->set_code(std::make_unique<IRCode>(callee, 0));

  auto caller = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "testCaller", "V", {}));
  caller->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  caller->set_code(std::make_unique<IRCode>(caller, 0));

  auto invoke = dasm(OPCODE_INVOKE_STATIC, callee, {});
  invoke->set_arg_word_count(2);
  invoke->set_src(0, 1);
  invoke->set_src(1, 2);

  auto caller_code = caller->get_code();
  caller_code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  caller_code->push_back(dasm(OPCODE_CONST, {2_v, 0_L})); // load null ref
  caller_code->push_back(invoke);
  auto invoke_it = std::prev(caller_code->end());
  caller_code->push_back(dasm(OPCODE_RETURN_VOID));
  caller_code->set_registers_size(3);

  auto callee_code = callee->get_code();
  callee_code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  callee_code->push_back(dasm(OPCODE_RETURN_VOID));

  inliner::inline_method(caller->get_code(), callee->get_code(), invoke_it);

  auto it = InstructionIterable(caller_code).begin();
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {1_v, 1_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {2_v, 0_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE, {3_v, 1_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE_OBJECT, {4_v, 2_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {4_v, 1_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_RETURN_VOID));

  EXPECT_EQ(caller_code->get_registers_size(), 5);
}

TEST_F(MethodInlineTest, debugPositionsAfterReturn) {
  DexMethod* caller =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.caller:()V"));
  caller->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:()V"));
  callee->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  const auto& caller_str = R"(
    (
      (.pos:dbg_0 "LFoo;.caller:()V" "Foo.java" 10)
      (const v0 0)
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (.pos:dbg_0 "LFoo;.callee:()V" "Foo.java" 123)
      (const v0 1)
      (if-eqz v0 :after)

      (:exit)
      (.pos:dbg_1 "LFoo;.callee:()V" "Foo.java" 124)
      (const v1 2)
      (return-void)

      (:after)
      (const v2 3)
      (goto :exit)
    )
  )";
  const auto& expected_str = R"(
    (
      (.pos:dbg_0 "LFoo;.caller:()V" "Foo.java" 10)
      (const v0 0)

      (.pos:dbg_1 "LFoo;.callee:()V" "Foo.java" 123 dbg_0)
      (const v1 1)
      (if-eqz v1 :after)

      (:exit)
      (.pos:dbg_2 "LFoo;.callee:()V" "Foo.java" 124 dbg_0)
      (const v2 2)
      (.pos:dbg_3 "LFoo;.caller:()V" "Foo.java" 10)
      (return-void)

      ; Check that this position was correctly added to the code after the
      ; callee's return
      (.pos:dbg_4 "LFoo;.callee:()V" "Foo.java" 124 dbg_0)
      (:after)
      (const v3 3)
      (goto :exit)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST_F(MethodInlineTest, test_within_dex_inlining) {
  MethodRefCache resolve_cache;
  auto resolver = [&resolve_cache](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, resolve_cache);
  };

  MultiMethodInliner::Config inliner_config;
  // Only inline methods within dex.
  inliner_config.within_dex = true;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> canidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  auto bar_cls = create_a_class("Lbar;");
  {
    // foo is in dex 2, bar is in dex 3.
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    store.add_classes({bar_cls});
    stores.push_back(std::move(store));
  }
  {
    auto foo_m1 = make_a_method(foo_cls, "foo_m1", 1);
    auto bar_m1 = make_a_method(bar_cls, "bar_m1", 2001);
    auto bar_m2 = make_a_method(bar_cls, "bar_m2", 2002);
    canidates.insert(foo_m1);
    canidates.insert(bar_m1);
    canidates.insert(bar_m2);
    // foo_main calls foo_m1 and bar_m2.
    auto foo_main =
        make_a_method_calls_others(foo_cls, "foo_main", {foo_m1, bar_m2});
    // bar_main calls bar_m1.
    auto bar_main = make_a_method_calls_others(bar_cls, "bar_main", {bar_m1});
    // Expect foo_m1 and bar_m1 be inlined if `within_dex` is true.
    expected_inlined.insert(foo_m1);
    expected_inlined.insert(bar_m1);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);

  MultiMethodInliner inliner(
      scope, stores, canidates, resolver, inliner_config);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }
}
