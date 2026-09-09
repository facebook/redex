/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "AtomicFieldUpdaters.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "Show.h"
#include "VerifyUtil.h"

namespace {

constexpr const char* kHolder =
    "Lcom/facebook/redex/test/instr/AtomicFieldUpdaterLoweringTest$Holder;";
constexpr const char* kTest =
    "Lcom/facebook/redex/test/instr/AtomicFieldUpdaterLoweringTest;";

// Counts invocations, by the class the invoked method belongs to, across every
// method of `cls`. An empty `member_name` counts every member of that class.
size_t count_invokes_to(DexClass* cls,
                        const std::string& owner_descriptor,
                        const std::string& member_name = "") {
  size_t n = 0;
  auto count = [&](DexMethod* m) {
    auto* code = m->get_dex_code();
    if (code == nullptr) {
      return;
    }
    for (auto* insn : code->get_instructions()) {
      if (!insn->has_method()) {
        continue;
      }
      // The method reference lives on the DexOpcodeMethod subclass; the base
      // DexInstruction only knows that a reference is present.
      const auto* mop = dynamic_cast<const DexOpcodeMethod*>(insn);
      if (mop != nullptr &&
          show(mop->get_method()->get_class()) == owner_descriptor &&
          (member_name.empty() ||
           show(mop->get_method()->get_name()) == member_name)) {
        n++;
      }
    }
  };
  for (auto* m : cls->get_dmethods()) {
    count(m);
  }
  for (auto* m : cls->get_vmethods()) {
    count(m);
  }
  return n;
}

size_t count_invokes_to(DexMethod* method,
                        const std::string& owner_descriptor) {
  auto* code = method->get_dex_code();
  if (code == nullptr) {
    return 0;
  }
  size_t n = 0;
  for (auto* insn : code->get_instructions()) {
    if (!insn->has_method()) {
      continue;
    }
    const auto* mop = dynamic_cast<const DexOpcodeMethod*>(insn);
    if (mop != nullptr &&
        show(mop->get_method()->get_class()) == owner_descriptor) {
      n++;
    }
  }
  return n;
}

// Counts invocations of one named `sun.misc.Unsafe` member across every method
// of `cls`. Expressed through `count_invokes_to` so the two cannot drift.
size_t count_invokes_to_unsafe_member(DexClass* cls,
                                      const std::string& member_name) {
  return count_invokes_to(cls, atomic_field_updaters::UNSAFE_DESC, member_name);
}

size_t count_invokes_to_methods_named_like(
    DexClass* cls,
    const std::string& method_name_fragment,
    const std::string& owner_descriptor) {
  size_t n = 0;
  auto count = [&](DexMethod* method) {
    if (show(method->get_name()).find(method_name_fragment) ==
        std::string::npos) {
      return;
    }
    n += count_invokes_to(method, owner_descriptor);
  };
  for (auto* method : cls->get_dmethods()) {
    count(method);
  }
  for (auto* method : cls->get_vmethods()) {
    count(method);
  }
  return n;
}

} // namespace

/*
 * Before Redex runs, the test class calls through the updaters.
 */
TEST_F(PreVerify, AtomicFieldUpdaterLowering) {
  auto* test = find_class_named(classes, kTest);
  ASSERT_NE(nullptr, test);

  EXPECT_GT(count_invokes_to(test, atomic_field_updaters::REFERENCE_DESC), 0u);
  EXPECT_GT(count_invokes_to(test, atomic_field_updaters::INTEGER_DESC), 0u);
  EXPECT_GT(count_invokes_to(test, atomic_field_updaters::LONG_DESC), 0u);
  // No Unsafe anywhere yet.
  EXPECT_EQ(count_invokes_to(test, atomic_field_updaters::UNSAFE_DESC), 0u);
}

/*
 * After Redex, those calls are Unsafe operations instead.
 *
 * Without this the JUnit half could pass vacuously: if the pass silently
 * stopped firing -- which is exactly how it failed once already, when Kotlin's
 * synthetic accessors hid every receiver -- the unrewritten code would still
 * behave correctly and every assertion would still be green.
 */
TEST_F(PostVerify, AtomicFieldUpdaterLowering) {
  auto* test = find_class_named(classes, kTest);
  ASSERT_NE(nullptr, test);

  EXPECT_GT(count_invokes_to(test, atomic_field_updaters::UNSAFE_DESC), 0u)
      << "no Unsafe calls after the pass: the lowering did not fire, so the "
         "on-device assertions prove nothing about the rewritten code";

  // Every lowerable updater operation in the test should have been lowered.
  // The two raw-typed negative tests intentionally keep their reference-updater
  // calls: those sites are exactly the wrong-holder / wrong-value shapes the
  // pass must leave in place so the JUnit half still exercises the library's
  // own runtime checks. javac/desugaring moves those calls into `lambda$...`
  // helpers, so match the compiled method-name fragment rather than the public
  // test method body.
  EXPECT_EQ(count_invokes_to_methods_named_like(
                test,
                "wrongValueTypeThrowsClassCastException",
                atomic_field_updaters::REFERENCE_DESC),
            1u);
  EXPECT_GE(count_invokes_to_methods_named_like(
                test,
                "wrongHolderTypeThrowsClassCastException",
                atomic_field_updaters::REFERENCE_DESC),
            1u);
  EXPECT_EQ(count_invokes_to(test, atomic_field_updaters::REFERENCE_DESC), 2u);

  // Android's non-SDK interface policy denies these four to app code whenever
  // the app targets past API 30, which this test now does and every shipping
  // app already did. They link at build time and throw NoSuchMethodError on
  // Android 12+, so emitting one is invisible until it reaches a device --
  // which is how T287786534 became a launch-blocking crash. This is the
  // assertion that would have caught it.
  for (const char* member :
       {"getAndAddInt", "getAndAddLong", "getAndSetInt", "getAndSetLong"}) {
    EXPECT_EQ(count_invokes_to_unsafe_member(test, member), 0u)
        << "the lowering emitted sun.misc.Unsafe." << member
        << ", which is max-target-r: this build cannot link it at runtime";
  }

  // Consequently the arithmetic forms over int and long, and the numeric
  // getAndSet, keep calling their updater. Bounded both ways rather than just
  // asserted non-zero: too few would mean a restricted member is being emitted
  // after all, too many that an allowed operation quietly stopped lowering,
  // and neither direction is caught by the checks above.
  //
  // These counts track the fixture. Adding or removing an arithmetic operation
  // in AtomicFieldUpdaterLoweringTest.java changes them, and that is a
  // deliberate update here, not a regression.
  constexpr size_t kIntegerCallsLeft = 8u;
  constexpr size_t kLongCallsLeft = 5u;
  EXPECT_EQ(count_invokes_to(test, atomic_field_updaters::INTEGER_DESC),
            kIntegerCallsLeft)
      << "integer updater calls left unlowered changed; if you edited the "
         "JUnit fixture's arithmetic operations, update kIntegerCallsLeft";
  EXPECT_EQ(count_invokes_to(test, atomic_field_updaters::LONG_DESC),
            kLongCallsLeft)
      << "long updater calls left unlowered changed; if you edited the JUnit "
         "fixture's arithmetic operations, update kLongCallsLeft";

  // The unrestricted operations on those same flavors still lower, so the
  // narrowing above is confined to the four members and has not quietly
  // switched the primitive flavors off altogether.
  EXPECT_GT(count_invokes_to_unsafe_member(test, "compareAndSwapInt"), 0u);
  EXPECT_GT(count_invokes_to_unsafe_member(test, "compareAndSwapLong"), 0u);
  EXPECT_GT(count_invokes_to_unsafe_member(test, "getIntVolatile"), 0u);
  EXPECT_GT(count_invokes_to_unsafe_member(test, "getLongVolatile"), 0u);
  // And the reference flavor keeps its getAndSet, which is unrestricted.
  EXPECT_GT(count_invokes_to_unsafe_member(test, "getAndSetObject"), 0u);

  // `lazySet` lowers to the ordered stores. These three are the members with
  // the least coverage elsewhere: an ordered write is not observable by
  // inspecting the caller, so if the JUnit half did not drive all three
  // flavors, a wrong width or a dropped store would pass everything.
  EXPECT_GT(count_invokes_to_unsafe_member(test, "putOrderedObject"), 0u);
  EXPECT_GT(count_invokes_to_unsafe_member(test, "putOrderedInt"), 0u);
  EXPECT_GT(count_invokes_to_unsafe_member(test, "putOrderedLong"), 0u);

  // The shared class supplies only the Unsafe instance. It must NOT carry the
  // offsets: computing one needs `Holder.class`, and this class sits in
  // package `redex` while a holder is often not public, so the lookup would
  // throw IllegalAccessError as this class initializes.
  auto* synth =
      find_class_named(classes, atomic_field_updaters::SYNTH_HOLDER_DESC);
  ASSERT_NE(nullptr, synth) << "the Unsafe holder class was not synthesized";
  for (auto* f : synth->get_sfields()) {
    EXPECT_NE(show(f->get_type()), "J")
        << "offset " << show(f)
        << " belongs in its own holder class, not the shared one";
  }

  auto* holder = find_class_named(classes, kHolder);
  ASSERT_NE(nullptr, holder);

  // Each offset lives on the class that owns the field it describes, so the
  // reflection happens inside that class and access control never applies.
  size_t offsets = 0;
  for (auto* f : holder->get_sfields()) {
    if (show(f->get_type()) == "J") {
      offsets++;
    }
  }
  EXPECT_GE(offsets, 3u)
      << "expected one offset field on the holder per updater it declares";

  // The volatile fields must survive under their original names: the injected
  // <clinit> code looks them up with getDeclaredField(String), so a rename
  // would become a NoSuchFieldException at class initialization.
  bool has_ref = false, has_i = false, has_l = false;
  for (auto* f : holder->get_ifields()) {
    const auto name = show(f->get_name());
    has_ref |= name == "ref";
    has_i |= name == "i";
    has_l |= name == "l";
  }
  EXPECT_TRUE(has_ref && has_i && has_l)
      << "a volatile field was renamed; getDeclaredField in the synthesized "
         "<clinit> would throw at runtime";
}
