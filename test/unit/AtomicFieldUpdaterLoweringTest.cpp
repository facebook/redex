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
  void run(const std::string& cls_name,
           const std::string& updater_desc,
           const std::string& field_name,
           const std::string& field_type,
           const std::vector<DexMethod*>& extra_methods = {}) {
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
    PassManager manager({&pass});
    ConfigFiles config(Json::nullValue);
    config.parse_global_config();
    DexStore store("classes");
    store.add_classes({cls});
    std::vector<DexStore> stores;
    stores.emplace_back(std::move(store));
    manager.run_passes(stores, config);

    // `get_metric` reads the *currently running* pass and is only valid during
    // a run; after `run_passes` the recorded metrics live in the pass info.
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

  int64_t recognized = -1;
  for (const auto& info : manager.get_pass_info()) {
    if (info.name.find("AtomicFieldUpdaterLowering") != std::string::npos) {
      auto it = info.metrics.find("updaters_recognized");
      if (it != info.metrics.end()) {
        recognized = it->second;
      }
    }
  }
  EXPECT_EQ(recognized, 0);
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
