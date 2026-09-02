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

} // namespace

class AtomicFieldUpdaterLoweringTest : public RedexTest {};

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
