/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Runs `ArtProfileWriterPass` through a real `PassManager` with never-compile
// enabled and a call-count threshold every method meets, so the only thing
// deciding whether a method is dropped from the baseline profile is its
// control flow: methods that loop are kept, because dropping one leaves its
// loop to the interpreter.

#include <string>

#include <gtest/gtest.h>
#include <json/value.h>

#include "ArtProfileWriterPass.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "Show.h"

class ArtProfileWriterNeverCompileTest : public RedexTest {
 public:
  // The number of methods the pass dropped from the default baseline profile,
  // for a scope holding one static method with the given body.
  int64_t never_compiled_methods(const std::string& body) {
    ClassCreator cc{DexType::make_type("LNeverCompile;")};
    cc.set_super(type::java_lang_Object());
    auto* m = DexMethod::make_method("LNeverCompile;.f:()V")
                  ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                  assembler::ircode_from_string(body),
                                  /* is_virtual */ false);
    m->set_deobfuscated_name(show(m));
    m->get_code()->build_cfg();
    cc.add_method(m);
    DexStore store("classes");
    store.add_classes({cc.create()});
    std::vector<DexStore> stores;
    stores.emplace_back(std::move(store));

    Json::Value json_conf;
    auto& harvest = json_conf["baseline_profile"]["default"]["harvest_config"];
    harvest["enable_never_compile"] = true;
    // Consider methods that are not in the profile, and select every method
    // that is called at most 0 times -- with no interactions, that is all of
    // them.
    harvest["never_compile_ignore_hot"] = true;
    harvest["never_compile_callcount_threshold"] = 0;

    ConfigFiles conf(json_conf);
    conf.parse_global_config();
    auto tmp_dir = redex::make_tmp_dir("apw_never_compile_%%%%%%%%");
    conf.set_outdir(tmp_dir.path);

    ArtProfileWriterPass pass;
    PassManager manager({&pass}, conf, RedexOptions{});
    manager.run_passes(stores, conf);

    for (const auto& info : manager.get_pass_info()) {
      if (info.pass == &pass) {
        auto it = info.metrics.find("never_compiled_methods");
        return it == info.metrics.end() ? -1 : it->second;
      }
    }
    ADD_FAILURE() << "ArtProfileWriterPass did not run";
    return -1;
  }
};

// The control: with the configuration above, a method without a loop is
// dropped. Without this, the tests below would pass on a pass that never drops
// anything.
TEST_F(ArtProfileWriterNeverCompileTest, methodWithoutLoopIsNeverCompiled) {
  EXPECT_EQ(never_compiled_methods(R"(
    (
      (const v0 1)
      (return-void)
    )
  )"),
            1);
}

TEST_F(ArtProfileWriterNeverCompileTest, methodWithLoopStaysCompiled) {
  EXPECT_EQ(never_compiled_methods(R"(
    (
      (const v0 3)
      (:loop)
      (add-int/lit v0 v0 -1)
      (if-gtz v0 :loop)
      (return-void)
    )
  )"),
            0);
}

// A cycle that can be entered at either of its two blocks runs as many times
// as any other loop, so it has to keep the method compiled just the same.
TEST_F(ArtProfileWriterNeverCompileTest, methodWithTwoEntryCycleStaysCompiled) {
  EXPECT_EQ(never_compiled_methods(R"(
    (
      (const v0 3)
      (if-eqz v0 :second)
      (:first)
      (add-int/lit v0 v0 1)
      (:second)
      (add-int/lit v0 v0 -1)
      (if-gtz v0 :first)
      (return-void)
    )
  )"),
            0);
}
