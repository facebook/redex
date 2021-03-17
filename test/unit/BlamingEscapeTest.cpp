/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlamingAnalysis.h"

#include "IRAssembler.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"

namespace ptrs = local_pointers::blaming;

namespace {

/**
 * Predicate that ESCAPES an instance of ptrs::BlameDomain, believes that the
 * value in question has escaped COUNT many times (an interval of type
 * ptrs::CountDomain), via the BLAMED instructions (a variable sized list of
 * IRInstruction pointers).
 */
#define EXPECT_ESCAPES(ESCAPES, COUNT, /* BLAMED */...)                    \
  do {                                                                     \
    const auto _escapes = (ESCAPES);                                       \
                                                                           \
    ASSERT_TRUE(_escapes.allocated());                                     \
                                                                           \
    const auto& _counts = _escapes.escape_counts();                        \
    const auto& _blamed = _escapes.to_blame();                             \
                                                                           \
    std::initializer_list<const IRInstruction*> _expected = {__VA_ARGS__}; \
                                                                           \
    EXPECT_EQ(_counts, (COUNT));                                           \
    EXPECT_EQ(_blamed.size(), _expected.size());                           \
                                                                           \
    for (const auto* _insn : _expected) {                                  \
      EXPECT_TRUE(_blamed.contains(_insn))                                 \
          << "Expecting " << SHOW(_insn) << " to blame for an escape.";    \
    }                                                                      \
  } while (false)

class BlamingEscapeTest : public RedexTest {};

TEST_F(BlamingEscapeTest, escapes) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)

    (new-instance "LBar;")
    (move-result-pseudo-object v1)

    (invoke-static (v0) "LFoo;.baz:(LFoo;)V")

    (iput-object v0 v1 "LBar;.foo:LFoo;")

    (iget-object v1 "LBar;.foo:LFoo;")
    (move-result-pseudo v2)

    (sput-object v1 "LFoo;.bar:LBar;")

    (invoke-virtual (v1 v0) "LBar;.qux:(LFoo;)V")

    (const v3 42)
    (invoke-static (v3) "LFoo;.quz:(I)V")

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;
  auto* const new_Bar = insns[2].insn;

  auto* const scall = insns[4].insn;
  auto* const iput = insns[5].insn;
  auto* const sput = insns[8].insn;
  auto* const vcall = insns[9].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo, new_Bar});

  EXPECT_EQ(escapes.size(), 2);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(3, 3),
                 /* BLAMED */ scall, iput, vcall);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Bar),
                 /* COUNT */ ptrs::CountDomain::finite(2, 2),
                 /* BLAMED */ sput, vcall);
}

TEST_F(BlamingEscapeTest, escapeThroughMove) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)
    (invoke-direct (v0) "LFoo;.<init>:()V")
    (move v1 v0)
    (return-object v1)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;
  auto* const init = insns[2].insn;
  auto* const ret = insns[insns.size() - 1].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(2, 2),
                 /* BLAMED */ init, ret);
}

TEST_F(BlamingEscapeTest, potentiallyNull) {
  auto code = assembler::ircode_from_string(R"((
    (load-param v0)
    (if-nez v0 :else)
      (const v1 0)
    (goto :end)
    (:else)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
    (:end)
    (return-object v1)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[4].insn;
  auto* const init = insns[6].insn;
  auto* const ret = insns[insns.size() - 1].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(2, 2),
                 /* BLAMED */ init, ret);
}

TEST_F(BlamingEscapeTest, mergedEscape) {
  auto code = assembler::ircode_from_string(R"((
    (load-param v0)
    (if-nez v0 :else)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
    (goto :end)
    (:else)
      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
    (:end)
    (return-object v1)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo_then = insns[2].insn;
  auto* const init_then = insns[4].insn;
  auto* const new_Foo_else = insns[6].insn;
  auto* const init_else = insns[8].insn;
  auto* const ret = insns[insns.size() - 1].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo_then, new_Foo_else});

  EXPECT_EQ(escapes.size(), 2);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo_then),
                 /* COUNT */ ptrs::CountDomain::finite(2, 2),
                 /* BLAMED */ init_then, ret);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo_else),
                 /* COUNT */ ptrs::CountDomain::finite(2, 2),
                 /* BLAMED */ init_else, ret);
}

TEST_F(BlamingEscapeTest, createAndEscapeInLoop) {
  auto code = assembler::ircode_from_string(R"((
    (:loop)
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "LFoo;.<init>:()V")
    (goto :loop)
    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;
  auto* const init = insns[2].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(1, 1),
                 /* BLAMED */ init);
}

TEST_F(BlamingEscapeTest, escapeInLoopAndAfter) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)
    (invoke-direct (v0) "LFoo;.<init>:()V")

    (new-instance "LBar;")
    (move-result-pseudo-object v1)
    (invoke-direct (v1) "LBar;.<init>:()V")

    (:loop)
      (invoke-static (v1) "LFoo;.baz:(LBar;)B")
      (move-result v2)
    (if-nez v2 :loop)

    (return-object v0)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;
  auto* const init_Foo = insns[2].insn;

  auto* const new_Bar = insns[3].insn;
  auto* const init_Bar = insns[5].insn;

  auto* const scall = insns[6].insn;
  auto* const ret = insns[insns.size() - 1].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo, new_Bar});

  EXPECT_EQ(escapes.size(), 2);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(2, 2),
                 /* BLAMED */ init_Foo, ret);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Bar),
                 /* COUNT */ ptrs::CountDomain::bounded_below(2),
                 /* BLAMED */ init_Bar, scall);
}

TEST_F(BlamingEscapeTest, filteredAllocators) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)

    (new-instance "LBar;")
    (move-result-pseudo-object v1)

    (invoke-static (v0) "LFoo;.baz:(LFoo;)V")

    (iput-object v0 v1 "LBar;.foo:LFoo;")

    (iget-object v1 "LBar;.foo:LFoo;")
    (move-result-pseudo v2)

    (sput-object v1 "LFoo;.bar:LBar;")

    (invoke-virtual (v1 v0) "LBar;.qux:(LFoo;)V")

    (const v3 42)
    (invoke-static (v3) "LFoo;.quz:(I)V")

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;

  auto* const scall = insns[4].insn;
  auto* const iput = insns[5].insn;
  auto* const vcall = insns[9].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(3, 3),
                 /* BLAMED */ scall, iput, vcall);
}

TEST_F(BlamingEscapeTest, safeMethods) {
  auto* const init = DexString::make_string("<init>");
  auto* const Bar_safe = DexMethod::make_method("LBar;.safe:(LBar;)LBar;");

  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)
    (invoke-direct (v0) "LFoo;.<init>:()V")

    (new-instance "LBar;")
    (move-result-pseudo-object v1)
    (invoke-direct (v1 v0) "LBar;.<init>:(LFoo;)V")

    ;; not allocator, not safe
    (invoke-static (v1) "LBar.unsafe:(LBar;)LBar;")
    (move-result-pseudo-object v2)

    ;; allocator, not safe
    (invoke-static (v2) "LBar;.unsafe:(LBar;)LBar;")
    (move-result-pseudo-object v3)

    ;; not allocator, safe
    (invoke-static (v3) "LBar;.safe:(LBar;)LBar;")
    (move-result-pseudo-object v4)

    ;; allocator, safe
    (invoke-static (v4) "LBar;.safe:(LBar;)LBar;")
    (move-result-pseudo-object v5)

    (return-object v5)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;
  auto* const new_Bar = insns[3].insn;

  auto* const Bar_v2 = insns[6].insn;
  auto* const Bar_v3 = insns[8].insn;
  auto* const Bar_v4 = insns[10].insn;
  auto* const Bar_v5 = insns[12].insn;
  auto* const ret = insns[14].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(
      *cfg, /* allocators */ {new_Foo, new_Bar, Bar_v3, Bar_v5},
      /* safe_methods */ {init, Bar_safe});

  EXPECT_EQ(escapes.size(), 4);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(0, 0),
                 /* BLAMED */);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Bar),
                 /* COUNT */ ptrs::CountDomain::finite(1, 1),
                 /* BLAMED */ Bar_v2);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(Bar_v3),
                 /* COUNT */ ptrs::CountDomain::finite(0, 0),
                 /* BLAMED */);

  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(Bar_v5),
                 /* COUNT */ ptrs::CountDomain::finite(1, 1),
                 /* BLAMED */ ret);
}

TEST_F(BlamingEscapeTest, notReachable) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)

    (goto :skip)
    (new-instance "LBar;")
    (move-result-pseudo-object v1)
    (:skip)

    (return-object v0)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;
  auto* const ret = insns[5].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(1, 1),
                 /* BLAMED */ ret);
}

TEST_F(BlamingEscapeTest, nonEscaping) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "LFoo;")
    (move-result-pseudo-object v0)

    (const v1 42)
    (iput v1 v0 "LFoo;.bar:I")

    (const v1 43)
    (iput v1 v0 "LFoo;.bar:I")

    (const v1 44)
    (iput v1 v0 "LFoo;.bar:I")

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[0].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(0, 0),
                 /* BLAMED */);
}

TEST_F(BlamingEscapeTest, optionalEscape) {
  auto code = assembler::ircode_from_string(R"((
    (load-param v0)

    (new-instance "LFoo;")
    (move-result-pseudo-object v1)

    (if-eqz v0 :skip)
    (sput-object v1 "LFoo;.bar:LFoo;")
    (:skip)

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[1].insn;
  auto* const sput = insns[4].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(0, 1),
                 /* BLAMED */ sput);
}

TEST_F(BlamingEscapeTest, nestedBranchesEscapeAtEnd) {
  auto code = assembler::ircode_from_string(R"((
    (load-param v0)
    (load-param v1)

    (new-instance "LFoo;")
    (move-result-pseudo-object v2)

    (if-eqz v0 :else-1)
      (if-eqz v1 :else-2)
        (const v3 42)
        (iput v3 v2 "LFoo;.bar:I")
      (goto :end-2)
      (:else-2)
        (const v3 43)
        (iput v3 v2 "LFoo;.bar:I")
      (:end-2)

      (const v3 44)
      (iput v3 v2 "LFoo;.baz:I")
    (goto :end-1)
    (:else-1)
      (const v3 45)
      (iput v3 v2 "LFoo;.baz:I")
    (:end-1)

    (new-instance "LBar;")
    (move-result-pseudo-object v4)

    (iput-object v2 v4 "LBar;.foo:LFoo;")

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[2].insn;
  auto* const iput = insns[insns.size() - 2].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(1, 1),
                 /* BLAMED */ iput);
}

TEST_F(BlamingEscapeTest, loops) {
  auto code = assembler::ircode_from_string(R"((
    (load-param v0)
    (new-instance "LFoo;")
    (move-result-pseudo-object v1)

    (const v2 0)
    (goto :check)
    (:continue)
      (invoke-static (v2 v1) "LFoo;.bar:(ILFoo;)V")
      (const v3 1)
      (add-int v2 v2 v3)
    (:check)
    (if-ne v0 v2 :continue)

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[1].insn;
  auto* const scall = insns[5].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::bounded_below(0),
                 /* BLAMED */ scall);
}

TEST_F(BlamingEscapeTest, diffBranchEscape) {
  auto code = assembler::ircode_from_string(R"((
    (load-param v0)
    (new-instance "LFoo;")
    (move-result-pseudo-object v1)

    (new-instance "LBar;")
    (move-result-pseudo-object v2)

    (if-eqz v0 :else)
      (iput-object v1 v2 "LBar;.baz:LFoo;")
    (goto :end)
    (:else)
      (iput-object v1 v2 "LBar;.qux:LFoo;")
    (:end)

    (return-void)
  ))");

  auto ii = InstructionIterable(code.get());
  std::vector<MethodItemEntry> insns{ii.begin(), ii.end()};

  auto* const new_Foo = insns[1].insn;
  auto* const put_baz = insns[insns.size() - 4].insn;
  auto* const put_qux = insns[insns.size() - 2].insn;

  cfg::ScopedCFG cfg(code.get());
  auto escapes = ptrs::analyze_escapes(*cfg, {new_Foo});

  EXPECT_EQ(escapes.size(), 1);
  EXPECT_ESCAPES(/* ESCAPES */ escapes.get(new_Foo),
                 /* COUNT */ ptrs::CountDomain::finite(1, 1),
                 /* BLAMED */ put_baz, put_qux);
}

} // namespace
