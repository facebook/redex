/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CFGInliner.h"
#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "ObjectInlinePlugin.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"

using namespace object_inliner_plugin;
cfg::InstructionIterator find_instruction_matching(cfg::ControlFlowGraph* cfg,
                                                   IRInstruction* i) {
  auto iterable = cfg::InstructionIterable(*cfg);
  for (auto it = iterable.begin(); it != iterable.end(); it++) {
    auto insn = it->insn;
    if (insn->opcode() == i->opcode()) {
      if (insn->srcs() != i->srcs() ||
          (insn->has_dest() && i->has_dest() && insn->dest() != i->dest()) ||
          (i->has_method() && insn->has_method() &&
           i->get_method() != insn->get_method()) ||
          (i->has_field() && insn->has_field() &&
           i->get_field() != insn->get_field())) {
        continue;
      }
      return it;
    }
  }
  always_assert_log(false, "can't find instruction %s in %s", SHOW(i),
                    SHOW(*cfg));
}

IRInstruction* find_put(cfg::ControlFlowGraph* cfg, DexFieldRef* field) {
  auto iterable = cfg::InstructionIterable(*cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (opcode::is_an_iput(it->insn->opcode()) &&
        field == it->insn->get_field()) {
      return it->insn;
    }
  }
  always_assert_log(false, "can't find input in %s", SHOW(*cfg));
}

void test_object_inliner(
    const std::string& caller_str,
    const std::string& callee_str,
    const std::string& callee_class,
    const std::string& caller_class,
    const std::string& insert_before_instr,
    uint16_t result_reg,
    uint16_t caller_this,
    const std::vector<std::pair<std::string, uint16_t>>& fields,
    const std::vector<std::pair<std::string, std::string>>& swap_fields,
    const std::vector<uint16_t>& srcs,
    const std::string& expected_str,
    boost::optional<const std::string> callee_ctor_str = boost::none) {
  DexType* callee_type = DexType::make_type(callee_class.c_str());
  DexType* caller_type = DexType::make_type(caller_class.c_str());

  std::vector<DexFieldRef*> field_refs = {};
  for (const auto& field_data : fields) {
    auto field_ref = DexField::make_field(callee_class + field_data.first);
    field_ref->make_concrete(ACC_PUBLIC);
    field_refs.emplace_back(field_ref);
  }

  std::unordered_map<DexFieldRef*, DexFieldRef*> field_swap_refs;
  for (const auto& field_swap : swap_fields) {
    auto callee_field = DexField::make_field(callee_class + field_swap.first);
    callee_field->make_concrete(ACC_PUBLIC);
    auto caller_field = DexField::make_field(caller_class + field_swap.second);
    caller_field->make_concrete(ACC_PUBLIC);
    field_swap_refs.emplace(callee_field, caller_field);
  }

  FieldSetMap field_map = {};

  auto field_b =
      DexField::make_field("LBaz;.wide:I")->make_concrete(ACC_PUBLIC);
  std::string final_cfg;

  auto caller_code = assembler::ircode_from_string(caller_str);
  {
    ScopedCFG caller(caller_code.get());

    auto callee_code = assembler::ircode_from_string(callee_str);
    callee_code->build_cfg(true);
    auto& callee = callee_code->cfg();

    for (size_t i = 0; i < fields.size(); i++) {
      auto field = field_refs[i];
      auto field_data = fields[i];
      field_map.emplace(
          field,
          (FieldSet){
              {field_data.second, {find_put(caller.get(), field)}},
          });
    }

    auto instr_code = assembler::ircode_from_string(insert_before_instr);
    auto insn = instr_code->begin()->insn;
    auto it = find_instruction_matching(caller.get(), insn);

    if (callee_ctor_str) {
      auto callee_ctor_code = assembler::ircode_from_string(*callee_ctor_str);
      ScopedCFG callee_ctor(callee_ctor_code.get());
      for (size_t i = 0; i < fields.size(); i++) {
        auto field = field_refs[i];
        auto field_data = fields[i];
        field_map.emplace(
            field,
            (FieldSet){
                {field_data.second, {find_put(callee_ctor.get(), field)}},
            });
      }

      ObjectInlinePlugin plugin = ObjectInlinePlugin(
          field_map, field_swap_refs, {0}, result_reg, 0, callee_type);
      cfg::CFGInliner::inline_cfg(caller.get(), it,
                                  /* needs_receiver_cast */ nullptr,
                                  *callee_ctor, caller->get_registers_size(),
                                  plugin);
      insn = instr_code->begin()->insn;
      it = find_instruction_matching(caller.get(), insn);
      cfg::CFGInliner::inline_cfg(caller.get(), it,
                                  /* needs_receiver_cast */ nullptr, callee,
                                  caller->get_registers_size(), plugin);
    } else {
      object_inliner_plugin::ObjectInlinePlugin plugin =
          object_inliner_plugin::ObjectInlinePlugin(
              field_map, field_swap_refs, {0}, result_reg, 0, callee_type);
      cfg::CFGInliner::inline_cfg(caller.get(), it,
                                  /* needs_receiver_cast */ nullptr, callee,
                                  caller->get_registers_size(), plugin);
    }

    caller->simplify();
    final_cfg = show(*caller);
  }
  caller_code->clear_cfg();
  auto expected_code = assembler::ircode_from_string(expected_str);

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(caller_code.get()))
      << final_cfg;
}

class ObjectInlinerTest : public RedexTest {};

TEST_F(ObjectInlinerTest, simple_class_inline) {
  const auto& caller_str = R"(
    (
    (load-param v0)
    (new-instance "LFoo;")
    (move-result-pseudo-object v1)
    (new-instance "LBar;")
    (move-result-pseudo-object v2)
    (.pos:0 "LBar;.fumble:()V" "Bar" "22")
    (invoke-virtual (v2 v1) "LBar;.child:(LFoo;)LBaz;")
    (return v2)
    )
  )";
  const auto& callee_str = R"(
    (
      (load-param v0)
      (new-instance "LBaz;")
      (move-result-pseudo-object v1)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (new-instance "LBar;")
      (move-result-pseudo-object v2)
      (.pos:dbg_0 "LBar;.fumble:()V" Bar 22)
      (nop)
      (move v3 v0)
      (new-instance "LBaz;")
      (move-result-pseudo-object v4)
      (move v2 v4)
      (invoke-virtual (v2 v1) "LBar;.child:(LFoo;)LBaz;")
      (return v2)
    )
  )";
  test_object_inliner(caller_str,
                      callee_str,
                      "LFoo;",
                      "LBar;",
                      "((invoke-virtual (v2 v1) \"LBar;.child:(LFoo;)LBaz;\"))",
                      2,
                      0,
                      {},
                      {},
                      {},
                      expected_str);
}

TEST_F(ObjectInlinerTest, simple_class_inline_with_cfg) {
  const auto& caller_str = R"(
    (
    (load-param v0)
    (new-instance "LFoo;")
    (move-result-pseudo-object v1)
    (new-instance "LBar;")
    (move-result-pseudo-object v2)
    (const v3 0)
    (if-eq v2 v3 :escape)
    (.pos:0 "LBar;.fumble:()V" "Bar" "22")
    (invoke-virtual (v2 v1) "LBar;.child:(LFoo;)LBaz;")
    (:escape)
    (return v2)
    )
  )";
  const auto& callee_str = R"(
    (
      (load-param v0)
      (new-instance "LBaz;")
      (move-result-pseudo-object v1)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (new-instance "LBar;")
      (move-result-pseudo-object v2)
      (const v3 0)
      (if-eq v2 v3 :L0)
      (.pos:dbg_0 "LBar;.fumble:()V" "Bar" "22")
      (nop)
      (move v4 v0)
      (new-instance "LBaz;")
      (move-result-pseudo-object v5)
      (move v2 v5)
      (invoke-virtual (v2 v1) "LBar;.child:(LFoo;)LBaz;")
      (:L0)
      (return v2)
    )
  )";
  test_object_inliner(caller_str,
                      callee_str,
                      "LFoo;",
                      "LBoo;",
                      "((invoke-virtual (v2 v1) \"LBar;.child:(LFoo;)LBaz;\"))",
                      2,
                      0,
                      {},
                      {},
                      {},
                      expected_str);
}

TEST_F(ObjectInlinerTest, class_inline_with_fields) {
  const auto& caller_str = R"(
    (
    (load-param v0)
    (load-param v1)
    (new-instance "LFoo;")
    (move-result-pseudo-object v2)
    (iput v1 v2 "LFoo;.prop:I")
    (new-instance "LBar;")
    (move-result-pseudo-object v3)
    (.pos:0 "LBar;.fumble:()V" "Bar" "22")
    (invoke-virtual (v3 v2) "LBar;.child:(LFoo;)LBaz;")
    (return v3)
    )
  )";
  const auto& callee_str = R"(
    ( (load-param v0)
      (.pos:1 "LFoo;.create:()V" "Foo" "23")
      (iget v0 "LFoo;.prop:I")
      (move-result-pseudo v1)
      (new-instance "LBaz;")
      (move-result-pseudo-object v2)
      (iput v1 v2 "LBaz;.wide:I")
      (return v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (load-param v1)
      (const v4 0)
      (new-instance "LFoo;")
      (move-result-pseudo-object v2)
      (move v4 v1)
      (new-instance "LBar;")
      (move-result-pseudo-object v3)
      (.pos:dbg_0 "LBar;.fumble:()V" Bar 22)
      (nop)
      (move v5 v0)
      (.pos:1 "LFoo;.create:()V" "Foo" "23" dbg_0)
      (move v6 v4)
      (new-instance "LBaz;")
      (move-result-pseudo-object v7)
      (iput v6 v7 "LBaz;.wide:I")
      (move v2 v7)
      (invoke-virtual (v3 v2) "LBar;.child:(LFoo;)LBaz;")
      (return v3)
    )
  )";
  test_object_inliner(caller_str,
                      callee_str,
                      "LFoo;",
                      "LBoo;",
                      "((invoke-virtual (v3 v2) \"LBar;.child:(LFoo;)LBaz;\"))",
                      2,
                      0,
                      {{".prop:I", 1}},
                      {},
                      {},
                      expected_str);
}

TEST_F(ObjectInlinerTest, class_inline_with_fields_and_swaps) {
  const auto& caller_str = R"(
    (
    (load-param v0)
    (load-param v1)
    (new-instance "LFoo;")
    (move-result-pseudo-object v2)
    (iput v1 v2 "LFoo;.prop:I")
    (new-instance "LBar;")
    (move-result-pseudo-object v3)
    (.pos:0 "LBar;.fumble:()V" "Bar" "22")
    (invoke-virtual (v3 v2) "LBar;.child:(LFoo;)LBaz;")
    (return v3)
    )
  )";
  const auto& callee_str = R"(
    ( (load-param v0)
      (.pos:1 "LFoo;.create:()V" "Foo" "23")
      (iget v0 "LFoo;.prop:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.nonprop:I")
      (move-result-pseudo v3)
      (new-instance "LBaz;")
      (move-result-pseudo-object v2)
      (iput v1 v2 "LBaz;.wide:I")
      (iput v1 v3 "LBaz;.push:I")
      (return v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (load-param v1)
      (const v4 0)
      (new-instance "LFoo;")
      (move-result-pseudo-object v2)
      (move v4 v1)
      (new-instance "LBar;")
      (move-result-pseudo-object v3)
      (.pos:dbg_0 "LBar;.fumble:()V" Bar 22)
      (nop)
      (move v5 v0)
      (.pos:1 "LFoo;.create:()V" "Foo" "23" dbg_0)
      (move v6 v4)
      (iget v0 "LBoo;.nonprop:I")
      (move-result-pseudo v8)
      (new-instance "LBaz;")
      (move-result-pseudo-object v7)
      (iput v6 v7 "LBaz;.wide:I")
      (iput v6 v8 "LBaz;.push:I")
      (move v2 v7)
      (invoke-virtual (v3 v2) "LBar;.child:(LFoo;)LBaz;")
      (return v3)
    )
  )";
  test_object_inliner(caller_str,
                      callee_str,
                      "LFoo;",
                      "LBoo;",
                      "((invoke-virtual (v3 v2) \"LBar;.child:(LFoo;)LBaz;\"))",
                      2,
                      0,
                      {{".prop:I", 1}},
                      {{".nonprop:I", ".nonprop:I"}},
                      {},
                      expected_str);
}

TEST_F(ObjectInlinerTest, full_class_inline_with_fields_and_swaps) {
  const auto& caller_str = R"(
    (
    (load-param v0)
    (load-param v1)
    (new-instance "LFoo;")
    (move-result-pseudo-object v2)
    (iput v1 v2 "LFoo;.prop:I")
    (new-instance "LBar;")
    (move-result-pseudo-object v3)
    (.pos:0 "LBar;.fumble:()V" "Bar" "22")
    (invoke-virtual (v3 v2) "LBar;.child:(LFoo;)LBaz;")
    (return v3)
    )
  )";

  // Constructor
  const std::string callee_ctor_str = R"(
    ( (load-param v0)
      (const v2 0)
      (iput v2 v0 "LFoo;.nonprop:I")
      (const v3 1)
      (iput v3 v0 "LFoo;.prop:I")
      (return-void)
    )
  )";

  const auto& callee_str = R"(
    ( (load-param v0)
      (.pos:1 "LFoo;.create:()V" "Foo" "23")
      (iget v0 "LFoo;.prop:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.nonprop:I")
      (move-result-pseudo v3)
      (new-instance "LBaz;")
      (move-result-pseudo-object v2)
      (iput v1 v2 "LBaz;.wide:I")
      (iput v1 v3 "LBaz;.push:I")
      (return v2)
    )
  )";

  const auto& expected_str = R"(
    (
      ;; "LFoo;.prop:I" gets v4. All writes and reads gets mapped to v4
      (load-param v0)
      (load-param v1)
      (const v4 0)
      (new-instance "LFoo;")
      (move-result-pseudo-object v2)
      (move v4 v1)
      (new-instance "LBar;")
      (move-result-pseudo-object v3)
      (.pos:dbg_0 "LBar;.fumble:()V" Bar 22)
      (nop)

      ;; Constructor
      (move v5 v0)
      (const v7 0)
      (iput v7 v0 "LBoo;.newnonprop:I")
      (const v8 1)
      (move v4 v8)

      ;; Other callee
      (move v9 v0)
      (.pos:1 "LFoo;.create:()V" "Foo" "23" dbg_0)
      (move v10 v4)
      (iget v0 "LBoo;.newnonprop:I")
      (move-result-pseudo v12)
      (new-instance "LBaz;")
      (move-result-pseudo-object v11)
      (iput v10 v11 "LBaz;.wide:I")
      (iput v10 v12 "LBaz;.push:I")
      (move v2 v11)
      (invoke-virtual (v3 v2) "LBar;.child:(LFoo;)LBaz;")
      (return v3)
    )
  )";
  test_object_inliner(caller_str,
                      callee_str,
                      "LFoo;",
                      "LBoo;",
                      "((invoke-virtual (v3 v2) \"LBar;.child:(LFoo;)LBaz;\"))",
                      2,
                      0,
                      {{".prop:I", 1}},
                      {{".nonprop:I", ".newnonprop:I"}},
                      {},
                      expected_str,
                      callee_ctor_str);
}
