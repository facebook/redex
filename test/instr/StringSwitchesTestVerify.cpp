/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ControlFlow.h"
#include "MethodUtil.h"
#include "StringSwitchTestUtil.h"
#include "verify/VerifyUtil.h"

using namespace testing;

namespace {

constexpr const char* SEARCH_MAP =
    "Lcom/facebook/common/dextricks/StringTreeSet;.searchMap:(Ljava/lang/"
    "String;Ljava/lang/String;I)I";

// Tallies the opcodes relevant to a String-switch rewrite in `method`,
// attributing searchMap lookup calls to OpcodeCounts::lookup.
string_switch_test::OpcodeCounts count(DexMethod* method) {
  method->balloon();
  return string_switch_test::count_string_switch_opcodes(
      InstructionIterable(method->get_code()),
      DexMethod::get_method(SEARCH_MAP));
}

// The catch types guarding a block, in catch order.
std::vector<const DexType*> handler_types(cfg::Block* block) {
  std::vector<const DexType*> types;
  for (auto* e : block->get_outgoing_throws_in_order()) {
    types.push_back(e->throw_info()->catch_type);
  }
  return types;
}

bool block_ends_with(cfg::Block* block, IROpcode op) {
  auto it = block->get_last_insn();
  return it != block->end() && it->insn->opcode() == op;
}

// The throw edges of both may-throw blocks the rewrite produces inside a try:
// the block running the lookup invoke, and the (goto-)preceding block holding
// the encoded string-tree const-string. Both must route to the original
// handlers.
struct LookupThrows {
  std::vector<const DexType*> invoke_handlers;
  std::vector<const DexType*> const_string_handlers;
  // The lookup invoke's goto-predecessor is expected to END in the payload
  // const-string (a throwing instruction must be last in its block).
  bool const_string_block_found{false};
};

// Builds a CFG so the reattached throw edges can be read directly from the
// transformed dex.
LookupThrows lookup_throws(DexMethod* method) {
  auto* searchmap_ref = DexMethod::get_method(SEARCH_MAP);
  method->balloon();
  auto* code = method->get_code();
  code->build_cfg();
  LookupThrows result;
  for (auto* b : code->cfg().blocks()) {
    if (!block_ends_with(b, OPCODE_INVOKE_STATIC) ||
        b->get_last_insn()->insn->get_method() != searchmap_ref) {
      continue;
    }
    result.invoke_handlers = handler_types(b);
    // The encoded const-string lives in the goto-predecessor (a throwing
    // instruction ends its block, so it cannot share the invoke's block).
    for (auto* e : b->preds()) {
      if (e->type() != cfg::EDGE_GOTO) {
        continue;
      }
      auto* pred = e->src();
      if (block_ends_with(pred, OPCODE_CONST_STRING)) {
        result.const_string_block_found = true;
        result.const_string_handlers = handler_types(pred);
      }
    }
    break;
  }
  code->clear_cfg();
  return result;
}

DexMethod* vmethod(const DexClasses& classes,
                   const char* cls_name,
                   const char* method_name) {
  auto* cls = find_class_named(classes, cls_name);
  EXPECT_NE(cls, nullptr) << cls_name;
  if (cls == nullptr) {
    return nullptr;
  }
  auto* m = find_vmethod_named(*cls, method_name);
  EXPECT_NE(m, nullptr) << cls_name << "." << method_name;
  return m;
}

DexMethod* dmethod(const DexClasses& classes,
                   const char* cls_name,
                   const char* method_name) {
  auto* cls = find_class_named(classes, cls_name);
  EXPECT_NE(cls, nullptr) << cls_name;
  if (cls == nullptr) {
    return nullptr;
  }
  auto* m = find_dmethod_named(*cls, method_name);
  EXPECT_NE(m, nullptr) << cls_name << "." << method_name;
  return m;
}

// Before the pass, a recoverable String switch decodes via hashCode()/equals()
// and there is no lookup call yet.
void expect_input_shape(DexMethod* method) {
  ASSERT_NE(method, nullptr);
  auto c = count(method);
  EXPECT_EQ(c.lookup, 0u) << show(method);
  EXPECT_GE(c.hashcode, 1u) << show(method);
  EXPECT_GE(c.equals, 1u) << show(method);
}

// After the pass, a transformed switch is a single lookup feeding one switch,
// with the hashCode()/equals() machinery removed.
void expect_transformed(DexMethod* method) {
  ASSERT_NE(method, nullptr);
  auto c = count(method);
  EXPECT_EQ(c.lookup, 1u) << show(method);
  EXPECT_EQ(c.switches, 1u) << show(method);
  EXPECT_EQ(c.hashcode, 0u) << show(method);
  EXPECT_EQ(c.equals, 0u) << show(method);
}

// After the pass, an ineligible switch is left exactly as it was: no lookup,
// and its equals() decode survives.
void expect_untouched(DexMethod* method) {
  ASSERT_NE(method, nullptr);
  auto c = count(method);
  EXPECT_EQ(c.lookup, 0u) << show(method);
  EXPECT_GE(c.equals, 1u) << show(method);
}

constexpr const char* EXAMPLE = "Lfoo/Example;";
constexpr const char* ANOTHER = "Lfoo/AnotherExample;";

} // namespace

TEST_F(PreVerify, VerifyInputState) {
  expect_input_shape(vmethod(classes, EXAMPLE, "big"));
  expect_input_shape(vmethod(classes, EXAMPLE, "handleValue"));
  expect_input_shape(vmethod(classes, EXAMPLE, "wrappedInTryCatch"));
  expect_input_shape(vmethod(classes, EXAMPLE, "wrappedInTryCatchMulti"));
  expect_input_shape(dmethod(classes, ANOTHER, "sameDestBlock"));

  // No lookup calls exist anywhere before the pass runs.
  EXPECT_EQ(count(vmethod(classes, EXAMPLE, "minimal")).lookup, 0u);
  EXPECT_EQ(count(dmethod(classes, ANOTHER, "lookup")).lookup, 0u);
}

TEST_F(PostVerify, VerifyTransformed) {
  // A plain HASH_SWITCH.
  expect_transformed(vmethod(classes, EXAMPLE, "big"));
  // Exactly at min_cases.
  expect_transformed(vmethod(classes, EXAMPLE, "handleValue"));
  // Inside a try with a single catch.
  expect_transformed(vmethod(classes, EXAMPLE, "wrappedInTryCatch"));
  // Inside a try with two catches.
  expect_transformed(vmethod(classes, EXAMPLE, "wrappedInTryCatchMulti"));
  // Many labels sharing one body.
  expect_transformed(dmethod(classes, ANOTHER, "sameDestBlock"));
}

TEST_F(PostVerify, VerifyTwoTransformsInOneMethod) {
  // Two independent switches -> two lookups feeding two switches.
  auto* method = vmethod(classes, EXAMPLE, "handleMultiple");
  ASSERT_NE(method, nullptr);
  auto c = count(method);
  EXPECT_EQ(c.lookup, 2u) << show(method);
  EXPECT_EQ(c.switches, 2u) << show(method);
  EXPECT_EQ(c.hashcode, 0u) << show(method);
  EXPECT_EQ(c.equals, 0u) << show(method);
}

TEST_F(PostVerify, VerifyUntouched) {
  // Below min_cases.
  expect_untouched(vmethod(classes, EXAMPLE, "minimal"));
  expect_untouched(dmethod(classes, ANOTHER, "lookup"));
  // A decoy with a mismatched hash bucket is not a recoverable String switch.
  expect_untouched(vmethod(classes, EXAMPLE, "decoy"));
}

// The exceptional control flow must survive the rewrite: BOTH may-throw blocks
// the rewrite introduces inside a try -- the encoded const-string block and the
// lookup invoke block -- must route to exactly the original handlers, with the
// same types in the same catch order. The matching JUnit test drives a null
// subject through these at runtime.
TEST_F(PostVerify, VerifyExceptionalEdges) {
  auto* exception = DexType::get_type("Ljava/lang/Exception;");
  auto* oom = DexType::get_type("Ljava/lang/OutOfMemoryError;");

  // Single catch: both the const-string and the lookup invoke -> Exception.
  auto single = lookup_throws(vmethod(classes, EXAMPLE, "wrappedInTryCatch"));
  EXPECT_EQ(single.invoke_handlers, std::vector<const DexType*>{exception});
  EXPECT_TRUE(single.const_string_block_found);
  EXPECT_EQ(single.const_string_handlers,
            std::vector<const DexType*>{exception});

  // Two catches, in order: OutOfMemoryError (tried first), then Exception. Both
  // throwing blocks must carry both handler edges in that order.
  auto multi =
      lookup_throws(vmethod(classes, EXAMPLE, "wrappedInTryCatchMulti"));
  std::vector<const DexType*> expected{oom, exception};
  EXPECT_EQ(multi.invoke_handlers, expected);
  EXPECT_TRUE(multi.const_string_block_found);
  EXPECT_EQ(multi.const_string_handlers, expected);
}
