/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "AtomicFieldUpdaterLoweringPass.h"
#include "AtomicFieldUpdaters.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRTemplate.h"
#include "PassManager.h"
#include "RedexTest.h"
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

  // Builds a class holding a volatile field and a `static final` updater over
  // it, initialized in <clinit>, plus any extra methods, then runs the pass.
  //
  // `updater_desc` selects the flavor. The reference flavor's `newUpdater`
  // takes (Class, Class, String); the Integer and Long flavors take
  // (Class, String) -- the field name therefore sits at a different argument
  // index, which is the recognizer's main flavor-specific concern.
  // Runs at min_sdk 24 unconditionally: that is where `getAndSet` and
  // `getAndAdd` become expressible at all, so any lower value would have the
  // API gate answer first and no test here would reach the behaviour it means
  // to pin. The sub-24 side of that gate belongs to the integ test
  // (AtomicFieldUpdaterApiGateTest), which drives both sides of the boundary.
  // `extra_classes` join the same store, for tests whose shape needs a second
  // class in the pass's scope rather than merely in the global type registry.
  void run(const std::string& cls_name,
           const std::string& updater_desc,
           const std::string& field_name,
           const std::string& field_type,
           const std::vector<DexMethod*>& extra_methods = {},
           const std::vector<DexClass*>& extra_classes = {}) {
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

    AtomicFieldUpdaterLoweringPass pass;
    ConfigFiles config(Json::nullValue);
    config.parse_global_config();
    RedexOptions options;
    options.min_sdk = 24;
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
