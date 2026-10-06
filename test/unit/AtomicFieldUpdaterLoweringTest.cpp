/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "AtomicFieldUpdaterLoweringPass.h"
#include "AtomicFieldUpdaters.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRTemplate.h"
#include "IRTypeChecker.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "TypeUtil.h"

namespace {

// The descriptors come from the shared service rather than being spelled out
// again here: a test asserting on a symbol it defines itself would keep passing
// if the definition the pass uses drifted.
using atomic_field_updaters::REFERENCE_DESC;

// A do-nothing constructor, so the assembled holder classes are well-formed.
// No test depends on its body.
constexpr const char* kInit = R"((
  (load-param-object v0)
  (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
  (return-void)
))";

// <clinit> building the updater. Two variants: the reference flavor's
// newUpdater takes (Class, Class, String), the Integer and Long flavors take
// (Class, String).
constexpr const char* kClinitReference = R"((
  (const-class "$CLS")
  (move-result-pseudo-object v0)
  (const-class "Ljava/lang/Object;")
  (move-result-pseudo-object v1)
  (const-string "$NAME")
  (move-result-pseudo-object v2)
  (invoke-static (v0 v1 v2) "$UPD.newUpdater:(Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/String;)$UPD")
  (move-result-object v3)
  (sput-object v3 "$CLS.U:$UPD")
  (return-void)
))";

constexpr const char* kClinitPrimitive = R"((
  (const-class "$CLS")
  (move-result-pseudo-object v0)
  (const-string "$NAME")
  (move-result-pseudo-object v2)
  (invoke-static (v0 v2) "$UPD.newUpdater:(Ljava/lang/Class;Ljava/lang/String;)$UPD")
  (move-result-object v3)
  (sput-object v3 "$CLS.U:$UPD")
  (return-void)
))";

} // namespace

class AtomicFieldUpdaterLoweringTest : public RedexTest {
 public:
  // Metrics recorded by the last `run()`.
  UnorderedMap<std::string, int64_t> metrics;
  // What `run()` passes as the app's min_sdk.
  int min_sdk{24};

  // `Class.name` of every method `method` invokes, one entry per call. The
  // pass manager tears the CFGs down after the last pass, hence the rebuild.
  static std::vector<std::string> invoked(DexMethod* method) {
    std::vector<std::string> out;
    cfg::ScopedCFG cfg(method->get_code());
    for (auto& mie : cfg::InstructionIterable(*cfg)) {
      if (mie.insn->has_method()) {
        const auto* mref = mie.insn->get_method();
        out.push_back(mref->get_class()->str_copy() + "." +
                      mref->get_name()->str_copy());
      }
    }
    return out;
  }

  static size_t count(const std::vector<std::string>& calls,
                      const std::string& target) {
    return std::count(calls.begin(), calls.end(), target);
  }

  // Builds a class holding a volatile field and a `static final` updater over
  // it, initialized in <clinit>, plus any extra methods, then runs the pass.
  //
  // `updater_desc` selects the flavor. The reference flavor's `newUpdater`
  // takes (Class, Class, String); the Integer and Long flavors take
  // (Class, String) -- the field name therefore sits at a different argument
  // index, which is the recognizer's main flavor-specific concern.
  // Runs at `min_sdk`, 24 unless a test sets it: that is where `getAndSet` and
  // `getAndAdd` become expressible at all, so any lower value would have the
  // API gate answer first and no test here would reach the behaviour it means
  // to pin. The sub-24 side of that gate belongs to the integ test
  // (AtomicFieldUpdaterApiGateTest), which drives both sides of the boundary.
  // Tests raise it to cross `kReferenceCasReliableMinSdk`.
  // `extra_classes` join the same store, for tests whose shape needs a second
  // class in the pass's scope rather than merely in the global type registry.
  // `configure` runs on the assembled holder just before the pass does, for
  // tests that need something this signature cannot express -- a keep bit on a
  // field, or a <clinit> of their own.
  void run(const std::string& cls_name,
           const std::string& updater_desc,
           const std::string& field_name,
           const std::string& field_type,
           const std::vector<DexMethod*>& extra_methods = {},
           const std::vector<DexClass*>& extra_classes = {},
           const std::function<void(DexClass*)>& configure = nullptr) {
    ClassCreator cc(DexType::make_type(cls_name));
    cc.set_super(type::java_lang_Object());
    cc.add_field(
        DexField::make_field(cls_name + "." + field_name + ":" + field_type)
            ->make_concrete(ACC_PUBLIC | ACC_VOLATILE));
    cc.add_field(DexField::make_field(cls_name + ".U:" + updater_desc)
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));

    const bool reference = updater_desc == REFERENCE_DESC;
    const std::string clinit_body =
        ir(reference ? kClinitReference : kClinitPrimitive,
           {{"$CLS", cls_name}, {"$UPD", updater_desc}, {"$NAME", field_name}});

    auto* clinit =
        DexMethod::make_method(cls_name + ".<clinit>:()V")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
    clinit->set_code(assembler::ircode_from_string(clinit_body));
    cc.add_method(clinit);

    auto* init = DexMethod::make_method(cls_name + ".<init>:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, false);
    init->set_code(assembler::ircode_from_string(kInit));
    cc.add_method(init);

    for (auto* m : extra_methods) {
      cc.add_method(m);
    }
    auto* cls = cc.create();
    if (configure) {
      configure(cls);
    }

    AtomicFieldUpdaterLoweringPass pass;
    ConfigFiles config(Json::nullValue);
    config.parse_global_config();
    RedexOptions options;
    options.min_sdk = min_sdk;
    PassManager manager({&pass}, config, options);
    DexStore store("classes");
    std::vector<DexClass*> in_store{cls};
    in_store.insert(in_store.end(), extra_classes.begin(), extra_classes.end());
    store.add_classes(in_store);
    std::vector<DexStore> stores;
    stores.emplace_back(std::move(store));
    manager.run_passes(stores, config);
    capture(manager);
  }

  // `get_metric` reads the *currently running* pass and is only valid during a
  // run; after `run_passes` the recorded metrics live in the pass info.
  void capture(PassManager& manager) {
    metrics.clear();
    for (const auto& info : manager.get_pass_info()) {
      if (info.name.find("AtomicFieldUpdaterLowering") != std::string::npos) {
        for (const auto& [k, v] : UnorderedIterable(info.metrics)) {
          metrics[k] = v;
        }
      }
    }
  }

  int64_t metric(const std::string& key) const {
    auto it = metrics.find(key);
    return it == metrics.end() ? -1 : it->second;
  }
};

// A register that held a newUpdater result and is then reloaded from somewhere
// else must not still be read as that result. Here `U` is assigned an updater
// copied from another class, so `U` does not describe `LRef;.next` at all --
// attributing it would make the lowering address that field through an offset
// belonging to a different one.
TEST_F(AtomicFieldUpdaterLoweringTest, registerReuseDoesNotMisattribute) {
  ClassCreator cc(DexType::make_type("LReuse;"));
  cc.set_super(type::java_lang_Object());
  cc.add_field(DexField::make_field("LReuse;.next:Ljava/lang/Object;")
                   ->make_concrete(ACC_PUBLIC | ACC_VOLATILE));
  cc.add_field(DexField::make_field(std::string("LReuse;.U:") + REFERENCE_DESC)
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));

  // v3 captures newUpdater(LReuse;, "next"), is overwritten by a load of an
  // unrelated updater, and only then stored into the candidate field.
  static constexpr const char* kClinit = R"((
    (const-class "LReuse;")
    (move-result-pseudo-object v0)
    (const-class "Ljava/lang/Object;")
    (move-result-pseudo-object v1)
    (const-string "next")
    (move-result-pseudo-object v2)
    (invoke-static (v0 v1 v2) "$UPD.newUpdater:(Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/String;)$UPD")
    (move-result-object v3)
    (sget-object "LElsewhere;.SHARED:$UPD")
    (move-result-pseudo-object v3)
    (sput-object v3 "LReuse;.U:$UPD")
    (return-void)
  ))";
  auto* clinit =
      DexMethod::make_method("LReuse;.<clinit>:()V")
          ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
  clinit->set_code(
      assembler::ircode_from_string(ir(kClinit, {{"$UPD", REFERENCE_DESC}})));
  cc.add_method(clinit);
  auto* cls = cc.create();

  AtomicFieldUpdaterLoweringPass pass;
  PassManager manager({&pass});
  ConfigFiles config(Json::nullValue);
  config.parse_global_config();
  DexStore store("classes");
  store.add_classes({cls});
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(store));
  manager.run_passes(stores, config);

  capture(manager);
  EXPECT_EQ(metric("updaters_recognized"), 0);
}

// The allow-list matches names, which says what the API calls an operation --
// not that this particular invoke has the API's signature. A zero-argument
// method named `get` on an updater is not `get(T)`: there is no holder to read,
// and reaching for one indexes a source that does not exist.
TEST_F(AtomicFieldUpdaterLoweringTest, wrongArityIsNotAnOperation) {
  static constexpr const char* kBadArity = R"((
    (sget-object "LRef;.U:$UPD")
    (move-result-pseudo-object v1)
    (invoke-virtual (v1) "$UPD.get:()Ljava/lang/Object;")
    (move-result-object v2)
    (return-void)
  ))";
  auto* m = DexMethod::make_method("LRef;.h:()V")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(kBadArity, {{"$UPD", REFERENCE_DESC}})));

  run("LRef;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {m});
  EXPECT_EQ(metric("rewritable_total"), 0);
  EXPECT_EQ(metric("feasible_total"), 0);
}

// A method the app has opted out of optimizing keeps its updater calls. The bit
// covers classes whose build-time bytecode does not match what runs, so a
// rewrite there is reasoning about code that will not be executed.
TEST_F(AtomicFieldUpdaterLoweringTest, noOptimizationsMethodIsNotRewritten) {
  static constexpr const char* kGet = R"((
    (load-param-object v0)
    (sget-object "LPinned;.U:$UPD")
    (move-result-pseudo-object v1)
    (invoke-virtual (v1 v0) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
    (move-result-object v2)
    (return-void)
  ))";
  auto* m = DexMethod::make_method("LPinned;.read:(LPinned;)V")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(kGet, {{"$UPD", REFERENCE_DESC}})));
  m->rstate.set_no_optimizations();

  run("LPinned;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {m});
  EXPECT_EQ(metric("updaters_recognized"), 1)
      << "recognized, just not rewritten";
  EXPECT_EQ(metric("calls_rewritten"), 0);
}

// A holder that is a strict subclass of the type the updater was created for is
// still a valid holder -- `accessCheck` tests `isInstance`, not identity -- so
// these sites must lower rather than be conservatively skipped.
TEST_F(AtomicFieldUpdaterLoweringTest, subclassHolderIsLowered) {
  // LSub; extends LBase;, and the call passes a LSub; where the updater names
  // LBase;.
  ClassCreator sub(DexType::make_type("LSub;"));
  sub.set_super(DexType::make_type("LBase;"));
  auto* sub_init = DexMethod::make_method("LSub;.<init>:()V")
                       ->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, false);
  // A concrete method with no code is not a shape any real dex has, and the
  // other assembled classes here are given a body for the same reason.
  sub_init->set_code(assembler::ircode_from_string(kInit));
  sub.add_method(sub_init);
  auto* sub_cls = sub.create();

  static constexpr const char* kGet = R"((
    (load-param-object v0)
    (sget-object "LBase;.U:$UPD")
    (move-result-pseudo-object v1)
    (invoke-virtual (v1 v0) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
    (move-result-object v2)
    (return-void)
  ))";
  auto* m = DexMethod::make_method("LBase;.read:(LSub;)V")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(kGet, {{"$UPD", REFERENCE_DESC}})));

  // `LSub;` joins the store so the subclass sits in the pass's scope, not only
  // in the global type registry.
  run("LBase;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {m}, {sub_cls});
  // `blocked_holder_type` is a trace counter, not a metric, so the observable
  // form of "the subclass was accepted" is that nothing was skipped as
  // unproven and the site was emitted.
  EXPECT_EQ(metric("calls_skipped_unproven_types"), 0);
  EXPECT_EQ(metric("calls_rewritten"), 1);
}

// Android's non-SDK interface policy is the reason four Unsafe members may not
// be named from app code. The table is the pass's only knowledge of it, so a
// member silently dropping out of it -- or an unvetted one being waved through
// -- is the regression this pins. See T287786534.
TEST_F(AtomicFieldUpdaterLoweringTest, hiddenApiTableClassifiesUnsafeMembers) {
  using atomic_field_updaters::hidden_api_status;
  using atomic_field_updaters::HiddenApiStatus;

  // max-target-r: the read-modify-write forms over int and long.
  for (const char* member :
       {"getAndAddInt", "getAndAddLong", "getAndSetInt", "getAndSetLong"}) {
    auto status = hidden_api_status(member);
    ASSERT_TRUE(status.has_value()) << member;
    EXPECT_EQ(*status, HiddenApiStatus::RESTRICTED) << member;
  }

  // The reference flavor's getAndSet sits beside the two restricted ones and is
  // not restricted. Asserted explicitly because the obvious over-correction is
  // to block the whole `getAndSet` family, which would cost reference sites for
  // nothing.
  for (const char* member :
       {"getAndSetObject", "compareAndSwapObject", "compareAndSwapInt",
        "compareAndSwapLong", "getObjectVolatile", "getIntVolatile",
        "getLongVolatile", "putObjectVolatile", "putIntVolatile",
        "putLongVolatile", "putOrderedObject", "putOrderedInt",
        "putOrderedLong", "objectFieldOffset"}) {
    auto status = hidden_api_status(member);
    ASSERT_TRUE(status.has_value()) << member;
    EXPECT_EQ(*status, HiddenApiStatus::ALLOWED) << member;
  }

  // An unclassified member is not implicitly permitted.
  EXPECT_FALSE(hidden_api_status("getAndBitwiseOrInt").has_value());
}

// A restricted member is recognized and left alone rather than emitted. Before
// this gate existed the pass emitted `Unsafe.getAndAddInt` here, which links
// fine at build time and throws NoSuchMethodError on Android 12+.
TEST_F(AtomicFieldUpdaterLoweringTest, restrictedMemberIsNotLowered) {
  using atomic_field_updaters::INTEGER_DESC;
  static constexpr const char* kIncrement = R"((
    (load-param-object v0)
    (sget-object "LCounter;.U:$UPD")
    (move-result-pseudo-object v1)
    (invoke-virtual (v1 v0) "$UPD.getAndIncrement:(Ljava/lang/Object;)I")
    (move-result v2)
    (return-void)
  ))";
  auto* m = DexMethod::make_method("LCounter;.bump:(LCounter;)V")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(kIncrement, {{"$UPD", INTEGER_DESC}})));

  run("LCounter;", INTEGER_DESC, "n", "I", {m});
  EXPECT_EQ(metric("updaters_recognized"), 1) << "found, just not emittable";
  EXPECT_EQ(metric("blocked_hidden_api"), 1);
  EXPECT_EQ(metric("blocked_min_sdk"), 0)
      << "min_sdk 24 supplies the member; the policy is what withholds it";
  EXPECT_EQ(metric("rewritable_total"), 0);
  EXPECT_EQ(metric("calls_rewritten"), 0);
}

// The same operation on the reference flavor lowers to `getAndSetObject`, which
// carries no restriction. The pair of tests is the point: a gate keyed on the
// updater operation rather than on the Unsafe member it resolves to would fail
// this one.
TEST_F(AtomicFieldUpdaterLoweringTest, referenceGetAndSetIsStillLowered) {
  static constexpr const char* kGetAndSet = R"((
    (load-param-object v0)
    (load-param-object v1)
    (sget-object "LRefSwap;.U:$UPD")
    (move-result-pseudo-object v2)
    (invoke-virtual (v2 v0 v1) "$UPD.getAndSet:(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")
    (move-result-object v3)
    (return-void)
  ))";
  auto* m =
      DexMethod::make_method("LRefSwap;.swap:(LRefSwap;Ljava/lang/Object;)V")
          ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(assembler::ircode_from_string(
      ir(kGetAndSet, {{"$UPD", REFERENCE_DESC}})));

  run("LRefSwap;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {m});
  EXPECT_EQ(metric("blocked_hidden_api"), 0);
  EXPECT_EQ(metric("rewritable_total"), 1);
  EXPECT_EQ(metric("calls_rewritten"), 1);
}

// A keep rule on the updater field survives the lowering. Cleanup removes a
// field once nothing reads it, and "nothing reads it" is not the same as "it
// may be deleted" -- a keep rule can pin a field precisely because something
// outside the dex, typically reflection, still names it.
TEST_F(AtomicFieldUpdaterLoweringTest, keptUpdaterFieldIsNotRemoved) {
  static constexpr const char* kGet = R"((
    (load-param-object v0)
    (sget-object "LKept;.U:$UPD")
    (move-result-pseudo-object v1)
    (invoke-virtual (v1 v0) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
    (move-result-object v2)
    (return-void)
  ))";
  auto* m = DexMethod::make_method("LKept;.read:(LKept;)V")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(kGet, {{"$UPD", REFERENCE_DESC}})));

  run("LKept;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {m}, {},
      [](DexClass* cls) {
        for (auto* f : cls->get_sfields()) {
          if (f->get_type() == DexType::get_type(REFERENCE_DESC)) {
            f->rstate.set_root();
          }
        }
      });

  // The site still lowers -- the keep bit is about deleting the field, not
  // about rewriting its uses.
  EXPECT_EQ(metric("calls_rewritten"), 1);
  EXPECT_EQ(metric("updater_fields_removed"), 0);
  EXPECT_GE(metric("cleanup_skipped_undeletable"), 1);

  auto* cls = type_class(DexType::get_type("LKept;"));
  ASSERT_NE(cls, nullptr);
  size_t updater_fields = 0;
  for (auto* f : cls->get_sfields()) {
    if (f->get_type() == DexType::get_type(REFERENCE_DESC)) {
      updater_fields++;
    }
  }
  EXPECT_EQ(updater_fields, 1u) << "a kept field must survive cleanup";
}

// An updater initialized inside a try region is left alone by cleanup. Removing
// the init slice would delete instructions the catch block was written for:
// `newUpdater` throws, and this shape exists precisely to handle that.
TEST_F(AtomicFieldUpdaterLoweringTest, updaterInitInTryRegionIsNotCleanedUp) {
  // try { U = newUpdater(...); } catch (Throwable t) { throw new
  // RuntimeException(); } The catch rethrows because that is the only shape
  // javac accepts for a final field: a swallowing catch leaves it unassigned,
  // and assigning in both arms leaves it possibly-already-assigned.
  static constexpr const char* kClinitTry = R"((
    (.try_start t)
    (const-class "LTryHolder;")
    (move-result-pseudo-object v0)
    (const-class "Ljava/lang/Object;")
    (move-result-pseudo-object v1)
    (const-string "next")
    (move-result-pseudo-object v2)
    (invoke-static (v0 v1 v2) "$UPD.newUpdater:(Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/String;)$UPD")
    (move-result-object v3)
    (sput-object v3 "LTryHolder;.U:$UPD")
    (.try_end t)
    (return-void)

    (.catch (t))
    (new-instance "Ljava/lang/RuntimeException;")
    (move-result-pseudo-object v4)
    (invoke-direct (v4) "Ljava/lang/RuntimeException;.<init>:()V")
    (throw v4)
  ))";
  static constexpr const char* kGet = R"((
    (load-param-object v0)
    (sget-object "LTryHolder;.U:$UPD")
    (move-result-pseudo-object v1)
    (invoke-virtual (v1 v0) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
    (move-result-object v2)
    (return-void)
  ))";
  auto* m = DexMethod::make_method("LTryHolder;.read:(LTryHolder;)V")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(kGet, {{"$UPD", REFERENCE_DESC}})));

  // Only the <clinit> differs from the standard holder, so swap that in rather
  // than assembling a second copy of the fixture by hand.
  run("LTryHolder;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {m}, {},
      [](DexClass* cls) {
        cls->get_clinit()->set_code(assembler::ircode_from_string(
            ir(kClinitTry, {{"$UPD", REFERENCE_DESC}})));
      });

  EXPECT_EQ(metric("updaters_recognized"), 1);
  // Exactly one refusal, and it is the updater's: the offset field is still
  // read by the site that lowered, so cleanup never considers removing it and
  // cannot be the source of this count.
  EXPECT_EQ(metric("cleanup_skipped_try_region"), 1);
  EXPECT_EQ(metric("updater_fields_removed"), 0);
  EXPECT_EQ(metric("updater_inits_removed"), 0);

  auto* cls = type_class(DexType::get_type("LTryHolder;"));
  ASSERT_NE(cls, nullptr);
  size_t updater_fields = 0;
  for (auto* f : cls->get_sfields()) {
    if (f->get_type() == DexType::get_type(REFERENCE_DESC)) {
      updater_fields++;
    }
  }
  EXPECT_EQ(updater_fields, 1u)
      << "an updater built under a catch handler must survive cleanup";
}

// -- The reference compareAndSet and d8's Android 12 forwarder -------------
//
// On Android 12 a reference compare-and-set can fail spuriously (b/211646483),
// the raw Unsafe primitive included. Below API 32 d8 never emits
// `AtomicReferenceFieldUpdater.compareAndSet`; it calls a synthetic forwarder
// that retries while the field still holds `expect`. These pin that only the
// genuine forwarder is recognized, that a strong reference CAS keeps retrying,
// and that nothing else pays for it.

namespace {

// d8's forwarder, as it emits it.
constexpr const char* kForwarder = R"((
  (load-param-object v0)
  (load-param-object v1)
  (load-param-object v2)
  (load-param-object v3)
  (:loop)
  (invoke-virtual (v0 v1 v2 v3) "$UPD.compareAndSet:(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
  (move-result v4)
  (if-eqz v4 :recheck)
  (const v5 1)
  (return v5)
  (:recheck)
  (invoke-virtual (v0 v1) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
  (move-result-object v6)
  (if-eq v6 v2 :loop)
  (const v5 0)
  (return v5)
))";

// The same loop with both tests inverted: another compiler's way of writing it.
constexpr const char* kForwarderInverted = R"((
  (load-param-object v0)
  (load-param-object v1)
  (load-param-object v2)
  (load-param-object v3)
  (:loop)
  (invoke-virtual (v0 v1 v2 v3) "$UPD.compareAndSet:(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
  (move-result v4)
  (if-nez v4 :done)
  (invoke-virtual (v0 v1) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
  (move-result-object v6)
  (if-ne v6 v2 :lost)
  (goto :loop)
  (:done)
  (const v5 1)
  (return v5)
  (:lost)
  (const v5 0)
  (return v5)
))";

// Retries while the field equals `update` rather than `expect`: not the
// operation, however close it looks.
constexpr const char* kForwarderWrongComparand = R"((
  (load-param-object v0)
  (load-param-object v1)
  (load-param-object v2)
  (load-param-object v3)
  (:loop)
  (invoke-virtual (v0 v1 v2 v3) "$UPD.compareAndSet:(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
  (move-result v4)
  (if-eqz v4 :recheck)
  (const v5 1)
  (return v5)
  (:recheck)
  (invoke-virtual (v0 v1) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
  (move-result-object v6)
  (if-eq v6 v3 :loop)
  (const v5 0)
  (return v5)
))";

// Reports the outcome inverted.
constexpr const char* kForwarderInvertedResult = R"((
  (load-param-object v0)
  (load-param-object v1)
  (load-param-object v2)
  (load-param-object v3)
  (:loop)
  (invoke-virtual (v0 v1 v2 v3) "$UPD.compareAndSet:(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
  (move-result v4)
  (if-eqz v4 :recheck)
  (const v5 0)
  (return v5)
  (:recheck)
  (invoke-virtual (v0 v1) "$UPD.get:(Ljava/lang/Object;)Ljava/lang/Object;")
  (move-result-object v6)
  (if-eq v6 v2 :loop)
  (const v5 1)
  (return v5)
))";

// A caller handing the forwarder a recognized updater.
constexpr const char* kCallForwarder = R"((
  (load-param-object v0)
  (load-param-object v1)
  (load-param-object v2)
  (sget-object "$CLS.U:$UPD")
  (move-result-pseudo-object v3)
  (invoke-static (v3 v0 v1 v2) "$FWD.m:($UPDLjava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
  (move-result v4)
  (return v4)
))";

// A caller invoking the updater directly, as code dexed for API 32+ would.
constexpr const char* kCallDirect = R"((
  (load-param-object v0)
  (load-param-object v1)
  (load-param-object v2)
  (sget-object "$CLS.U:$UPD")
  (move-result-pseudo-object v3)
  (invoke-virtual (v3 v0 v1 v2) "$UPD.$OP:(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
  (move-result v4)
  (return v4)
))";

const std::string kRetryHelper =
    std::string(atomic_field_updaters::SYNTH_HOLDER_DESC) + "." +
    atomic_field_updaters::CAS_RETRY_METHOD_NAME;
const std::string kRawCas = "Lsun/misc/Unsafe;.compareAndSwapObject";

// The class d8 would have put the forwarder `m` in.
DexClass* make_forwarder(const std::string& cls_name,
                         const char* body,
                         bool with_clinit = false) {
  ClassCreator cc(DexType::make_type(cls_name));
  cc.set_super(type::java_lang_Object());
  cc.set_access(ACC_PUBLIC | ACC_FINAL | ACC_SYNTHETIC);
  auto* m = DexMethod::make_method(
                cls_name + ".m:(" + REFERENCE_DESC +
                "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Z")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_SYNTHETIC, false);
  m->set_code(
      assembler::ircode_from_string(ir(body, {{"$UPD", REFERENCE_DESC}})));
  cc.add_method(m);
  if (with_clinit) {
    auto* clinit =
        DexMethod::make_method(cls_name + ".<clinit>:()V")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
    clinit->set_code(assembler::ircode_from_string("((return-void))"));
    cc.add_method(clinit);
  }
  return cc.create();
}

// `cls_name.cas(cls_name, Object, Object) -> boolean` with `body`.
DexMethod* make_caller(
    const std::string& cls_name,
    const char* body,
    const std::vector<std::pair<std::string_view, std::string>>& subs) {
  auto* m = DexMethod::make_method(cls_name + ".cas:(" + cls_name +
                                   "Ljava/lang/Object;Ljava/lang/Object;)Z")
                ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  m->set_code(assembler::ircode_from_string(ir(body, subs)));
  return m;
}

} // namespace

// d8's shape is recognized, and the calls to it are counted: they are where the
// reference compareAndSet went.
TEST_F(AtomicFieldUpdaterLoweringTest, forwarderCallIsRecognizedAndCounted) {
  auto* fwd = make_forwarder("LFwdA;", kForwarder);
  auto* caller = make_caller(
      "LCasA;", kCallForwarder,
      {{"$CLS", "LCasA;"}, {"$UPD", REFERENCE_DESC}, {"$FWD", "LFwdA;"}});

  run("LCasA;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller}, {fwd});
  EXPECT_EQ(metric("backport_cas_forwarders_recognized"), 1);
  EXPECT_EQ(metric("backport_cas_forwarders_rejected"), 0);
  EXPECT_EQ(metric("ops_backport_cas_calls"), 1);
}

// Recognition rests on what the body does, not on how a compiler laid it out.
TEST_F(AtomicFieldUpdaterLoweringTest, invertedForwarderIsRecognized) {
  auto* fwd = make_forwarder("LFwdC;", kForwarderInverted);
  auto* caller = make_caller(
      "LCasC;", kCallForwarder,
      {{"$CLS", "LCasC;"}, {"$UPD", REFERENCE_DESC}, {"$FWD", "LFwdC;"}});

  run("LCasC;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller}, {fwd});
  EXPECT_EQ(metric("backport_cas_forwarders_recognized"), 1);
}

// A method shaped like the forwarder that retries on the wrong value is not
// the operation. Treating it as one would change what the call returns.
TEST_F(AtomicFieldUpdaterLoweringTest, forwarderWithWrongComparandIsRejected) {
  auto* fwd = make_forwarder("LFwdD;", kForwarderWrongComparand);
  auto* caller = make_caller(
      "LCasD;", kCallForwarder,
      {{"$CLS", "LCasD;"}, {"$UPD", REFERENCE_DESC}, {"$FWD", "LFwdD;"}});

  run("LCasD;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller}, {fwd});
  EXPECT_EQ(metric("backport_cas_forwarders_recognized"), 0);
  EXPECT_EQ(metric("backport_cas_forwarders_rejected"), 1);
}

// Likewise one that reports the outcome inverted.
TEST_F(AtomicFieldUpdaterLoweringTest, forwarderWithInvertedResultIsRejected) {
  auto* fwd = make_forwarder("LFwdE;", kForwarderInvertedResult);
  auto* caller = make_caller(
      "LCasE;", kCallForwarder,
      {{"$CLS", "LCasE;"}, {"$UPD", REFERENCE_DESC}, {"$FWD", "LFwdE;"}});

  run("LCasE;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller}, {fwd});
  EXPECT_EQ(metric("backport_cas_forwarders_rejected"), 1);
}

// Replacing the call drops the initialization of the forwarder's class, so a
// class with an initializer to run is not a forwarder however exact its body.
TEST_F(AtomicFieldUpdaterLoweringTest,
       forwarderWithClassInitializerIsRejected) {
  auto* fwd = make_forwarder("LFwdF;", kForwarder, /*with_clinit=*/true);
  auto* caller = make_caller(
      "LCasF;", kCallForwarder,
      {{"$CLS", "LCasF;"}, {"$UPD", REFERENCE_DESC}, {"$FWD", "LFwdF;"}});

  run("LCasF;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller}, {fwd});
  EXPECT_EQ(metric("backport_cas_forwarders_rejected"), 1);
}

// A direct strong reference CAS has to keep retrying where the primitive can
// fail spuriously: below the reliable API it must not lower to the raw
// primitive.
TEST_F(AtomicFieldUpdaterLoweringTest, directCasRetriesBelowReliableMinSdk) {
  auto* caller = make_caller(
      "LCasG;", kCallDirect,
      {{"$CLS", "LCasG;"}, {"$UPD", REFERENCE_DESC}, {"$OP", "compareAndSet"}});

  run("LCasG;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller});
  EXPECT_EQ(metric("cas_retry_calls_emitted"), 1);
  auto calls = invoked(caller);
  EXPECT_EQ(count(calls, kRetryHelper), 1u);
  EXPECT_EQ(count(calls, kRawCas), 0u);
}

TEST_F(AtomicFieldUpdaterLoweringTest, directCasIsRawAtReliableMinSdk) {
  min_sdk = atomic_field_updaters::kReferenceCasReliableMinSdk;
  auto* caller = make_caller(
      "LCasH;", kCallDirect,
      {{"$CLS", "LCasH;"}, {"$UPD", REFERENCE_DESC}, {"$OP", "compareAndSet"}});

  run("LCasH;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller});
  EXPECT_EQ(metric("cas_retry_calls_emitted"), 0);
  EXPECT_EQ(count(invoked(caller), kRawCas), 1u);
}

// The weak form may fail spuriously by contract, so it never needs the retry.
TEST_F(AtomicFieldUpdaterLoweringTest, weakCasStaysRawBelowReliableMinSdk) {
  auto* caller = make_caller("LCasI;", kCallDirect,
                             {{"$CLS", "LCasI;"},
                              {"$UPD", REFERENCE_DESC},
                              {"$OP", "weakCompareAndSet"}});

  run("LCasI;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller});
  EXPECT_EQ(metric("cas_retry_calls_emitted"), 0);
  auto calls = invoked(caller);
  EXPECT_EQ(count(calls, kRawCas), 1u);
  EXPECT_EQ(count(calls, kRetryHelper), 0u);
}

// The helper is the retry: one swap, one re-read, and the re-read loops back to
// the swap. Type-checked, since nothing else verifies a synthesized body before
// it reaches a device.
TEST_F(AtomicFieldUpdaterLoweringTest, retryHelperIsALoopAroundTheSwap) {
  auto* caller = make_caller(
      "LCasJ;", kCallDirect,
      {{"$CLS", "LCasJ;"}, {"$UPD", REFERENCE_DESC}, {"$OP", "compareAndSet"}});
  run("LCasJ;", REFERENCE_DESC, "next", "Ljava/lang/Object;", {caller});

  auto* helper_ref = DexMethod::get_method(
      kRetryHelper +
      ":(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z");
  ASSERT_NE(helper_ref, nullptr);
  ASSERT_TRUE(helper_ref->is_def());
  auto* helper = helper_ref->as_def();

  IRTypeChecker checker(helper);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();

  auto calls = invoked(helper);
  EXPECT_EQ(count(calls, kRawCas), 1u);
  EXPECT_EQ(count(calls, "Lsun/misc/Unsafe;.getObjectVolatile"), 1u);

  cfg::ScopedCFG scoped(helper->get_code());
  auto& cfg = *scoped;
  auto block_calling = [&](const std::string& target) -> cfg::Block* {
    for (auto* block : cfg.blocks()) {
      for (auto& mie : ir_list::InstructionIterable(block)) {
        if (mie.insn->has_method() &&
            mie.insn->get_method()->get_name()->str() == target) {
          return block;
        }
      }
    }
    return nullptr;
  };
  auto* swap = block_calling("compareAndSwapObject");
  auto* reread = block_calling("getObjectVolatile");
  ASSERT_NE(swap, nullptr);
  ASSERT_NE(reread, nullptr);
  const auto& succs = reread->succs();
  EXPECT_TRUE(std::any_of(succs.begin(), succs.end(), [&](const cfg::Edge* e) {
    return e->target() == swap;
  })) << "an unchanged field must retry the swap";

  // Which way each test goes: a failed swap re-reads, a successful one returns
  // true; an unchanged field retries, a changed one returns false.
  auto taken = [&](cfg::Block* b) {
    auto* e = cfg.get_succ_edge_of_type(b, cfg::EDGE_BRANCH);
    return e == nullptr ? nullptr : e->target();
  };
  auto returned = [&](cfg::Block* b) -> std::optional<int64_t> {
    auto* e = cfg.get_succ_edge_of_type(b, cfg::EDGE_GOTO);
    if (e == nullptr) {
      return std::nullopt;
    }
    for (auto& mie : ir_list::InstructionIterable(e->target())) {
      if (mie.insn->opcode() == OPCODE_CONST) {
        return mie.insn->get_literal();
      }
    }
    return std::nullopt;
  };
  EXPECT_EQ(taken(swap), reread);
  EXPECT_EQ(returned(swap), 1);
  EXPECT_EQ(taken(reread), swap);
  EXPECT_EQ(returned(reread), 0);
}
