/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Purity.h"
#include "RedexTest.h"
#include "Show.h"
#include "ThrowPropagationImpl.h"
#include "VirtualScope.h"
#include "Walkers.h"

class ThrowPropagationTest : public RedexTest {
 public:
  ThrowPropagationTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    virt_scope::get_vmethods(type::java_lang_Object());
  }
};

bool exclude_method(DexMethod* method) {
  return method->get_code() == nullptr || is_abstract(method) ||
         method->is_external() || is_native(method) ||
         method->rstate.no_optimizations();
}

bool is_no_return_method(DexMethod* method) {
  if (exclude_method(method)) {
    return false;
  }
  bool can_return{false};
  editable_cfg_adapter::iterate_with_iterator(
      method->get_code(), [&can_return](const IRList::iterator& it) {
        if (opcode::is_a_return(it->insn->opcode())) {
          can_return = true;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return !can_return;
}

ConcurrentSet<DexMethod*> get_no_return_methods(const Scope& scope) {
  ConcurrentSet<DexMethod*> concurrent_no_return_methods;
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (is_no_return_method(method)) {
      concurrent_no_return_methods.insert(method);
    }
  });
  return concurrent_no_return_methods;
}

void run_throw_propagation(const ConcurrentSet<DexMethod*>& no_return_methods,
                           const method_override_graph::Graph& graph,
                           IRCode* code) {
  auto& cfg = code->cfg();
  std::vector<DexMethod*> return_methods;
  auto is_no_return_invoke = [&](IRInstruction* insn) {
    if (!opcode::is_an_invoke(insn->opcode()) ||
        insn->opcode() == OPCODE_INVOKE_SUPER) {
      return false;
    }
    auto method_ref = insn->get_method();
    DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
    if (method == nullptr) {
      return false;
    }
    if (insn->opcode() == OPCODE_INVOKE_INTERFACE &&
        is_annotation(type_class(method->get_class()))) {
      return false;
    }
    return_methods.clear();
    auto check_for_no_return = [&](DexMethod* other_method) {
      if (exclude_method(other_method)) {
        return false;
      }
      if (!no_return_methods.count_unsafe(other_method)) {
        return_methods.push_back(other_method);
      }
      return true;
    };
    if (!process_base_and_overriding_methods(
            &graph, method, /* methods_to_ignore */ nullptr,
            /* ignore_methods_with_assumenosideeffects */ false,
            check_for_no_return)) {
      return false;
    }
    return return_methods.empty();
  };

  throw_propagation_impl::ThrowPropagator impl(cfg);
  size_t throws_inserted{0};
  for (auto block : cfg.blocks()) {
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto insn = it->insn;
      if (!is_no_return_invoke(insn)) {
        continue;
      }

      if (impl.try_apply(block->to_cfg_instruction_iterator(it))) {
        throws_inserted++;
      }

      // Stop processing more instructions in this block
      break;
    }
  }

  if (throws_inserted > 0) {
    cfg.remove_unreachable_blocks();
    cfg.recompute_registers_size();
  }
}

void test(const Scope& scope,
          const std::string& code_str,
          const std::string& expected_str) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  auto no_return_methods = get_no_return_methods(scope);
  auto override_graph = method_override_graph::build_graph(scope);
  code->build_cfg();
  run_throw_propagation(no_return_methods, *override_graph, code.get());
  code->clear_cfg();

  EXPECT_CODE_EQ(code.get(), expected.get());
};

TEST_F(ThrowPropagationTest, dont_change_unknown) {
  auto code_str = R"(
    (
      (invoke-static () "LWhat;.ever:()V")
      (return-void)
    )
  )";
  test(Scope{type_class(type::java_lang_Object())}, code_str, code_str);
}

TEST_F(ThrowPropagationTest, can_return_simple) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       code_str);
}

TEST_F(ThrowPropagationTest, cannot_return_simple) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  auto expected_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (const v0 0)
      (throw v0)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       expected_str);
}

TEST_F(ThrowPropagationTest, cannot_return_remove_move_result) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()I")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()I")
      (move-result v1)
      (return-void)
    )
  )";
  auto expected_str = R"(
    (
      (invoke-static () "LFoo;.bar:()I")
      (const v2 0)
      (throw v2)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       expected_str);
}

TEST_F(ThrowPropagationTest, cannot_return_simple_already_throws) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (const v0 0)
      (throw v0)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       code_str);
}

TEST_F(ThrowPropagationTest, cannot_return_simple_already_does_not_terminate) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (:b)
      (nop)
      (goto :b)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       code_str);
}

TEST_F(ThrowPropagationTest, dont_change_throw_result) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()Ljava/lang/Exception;")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (return-object v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()Ljava/lang/Exception;")
      (move-result-object v0)
      (throw v0)
    )
  )";
  auto expected_str = code_str;
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       expected_str);
}
