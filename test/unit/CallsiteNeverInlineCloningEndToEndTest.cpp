/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Genuine two-pass proof: runs the actual, unmodified
// `CallsiteNeverInlineCloningPass::run_pass` and
// `ArtProfileWriterPass::run_pass` back to back through a real
// `PassManager`/`ConfigFiles`, on a hand-built (no dexfile, no BUCK
// def_integ_test) fixture -- so this fails if the clone ever stops reaching
// `ArtProfileWriterPass`'s own `hot_cold_callees` bucket, or if that pass's
// own annotation logic ever starts vetoing it, not just if the two passes'
// shape-eligibility checks (already covered by
// `never_inline_analysis::never_inline_eligibility`'s own tests) disagree.

#include <gtest/gtest.h>
#include <json/value.h>

#include "ArtProfileWriterPass.h"
#include "CallsiteNeverInlineCloningPass.h"
#include "Creators.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "Show.h"

namespace {

DexMethod* find_invoked_method(DexMethod* caller) {
  // `PassManager::run_passes` clears every method's CFG back to linear IRList
  // form once every pass has finished (so the pipeline can proceed to DEX
  // output); a caller inspecting the result after `run_passes` returns must
  // walk that linear form, not a (by then absent) CFG.
  for (auto& mie : InstructionIterable(*caller->get_code())) {
    if (opcode::is_an_invoke(mie.insn->opcode())) {
      return mie.insn->get_method()->as_def();
    }
  }
  return nullptr;
}

// A callee reached from one hot and two cold call sites -- "mixed-hotness"
// in this pass's own terms -- built directly with `ClassCreator`, entirely
// independent of any dexfile or BUCK `def_integ_test` fixture.
struct MixedHotnessFixture {
  DexMethod* callee;
  DexMethod* hot_caller;
  DexMethod* cold_caller1;
  DexMethod* cold_caller2;
  std::vector<DexStore> stores;
};

MixedHotnessFixture make_mixed_hotness_fixture() {
  ClassCreator cc{DexType::make_type("LCnicE2e;")};
  cc.set_super(type::java_lang_Object());

  // A sizable, non-trivial body -- shape-eligible for both passes (has a
  // return block, within the code-unit/instruction bounds, not a trivial
  // forwarder).
  std::ostringstream callee_code;
  callee_code << "(\n  (.src_block \"LCnicE2e;.callee:()V\" 0 (0.7 60))\n"
                 "  (const v0 1)\n";
  for (size_t i = 0; i < 14; i++) {
    callee_code << "  (add-int v0 v0 v0)\n";
  }
  callee_code << "  (return-void)\n)";
  auto* callee =
      DexMethod::make_method("LCnicE2e;.callee:()V")
          ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                          assembler::ircode_from_string(callee_code.str()),
                          false);
  callee->set_deobfuscated_name(show(callee));
  callee->get_code()->build_cfg();
  cc.add_method(callee);

  auto* hot_caller =
      DexMethod::make_method("LCnicE2e;.hot_caller:()V")
          ->make_concrete(
              ACC_PUBLIC | ACC_STATIC,
              assembler::ircode_from_string(
                  "(\n  (.src_block \"LCnicE2e;.hot_caller:()V\" 0 (1.0 100))\n"
                  "  (invoke-static () \"LCnicE2e;.callee:()V\")\n"
                  "  (return-void)\n)"),
              false);
  hot_caller->set_deobfuscated_name(show(hot_caller));
  hot_caller->get_code()->build_cfg();
  cc.add_method(hot_caller);

  auto make_cold_caller = [&](const std::string& name) {
    auto* m = DexMethod::make_method("LCnicE2e;." + name + ":()V")
                  ->make_concrete(
                      ACC_PUBLIC | ACC_STATIC,
                      assembler::ircode_from_string(
                          "(\n  (.src_block \"LCnicE2e;." + name +
                          ":()V\" 0 (0 0))\n"
                          "  (invoke-static () \"LCnicE2e;.callee:()V\")\n"
                          "  (return-void)\n)"),
                      false);
    m->set_deobfuscated_name(show(m));
    m->get_code()->build_cfg();
    cc.add_method(m);
    return m;
  };
  auto* cold_caller1 = make_cold_caller("cold_caller1");
  auto* cold_caller2 = make_cold_caller("cold_caller2");

  DexStore store("classes");
  store.add_classes({cc.create()});
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(store));

  return MixedHotnessFixture{callee, hot_caller, cold_caller1, cold_caller2,
                             std::move(stores)};
}

// `ConfigFiles::get_baseline_profile_configs()` always carries a "default"
// entry (`ConfigFiles.cpp` inserts one unconditionally when no
// `baseline_profile` json key is present), and
// `baseline_profiles::get_baseline_profiles()` derives its "manual" baseline
// profile for that entry from three fixed, config-independent
// method-profiles interaction ids ("manual_startup"/"manual_post_startup"/
// "manual_hot") -- exactly the mechanism this helper drives, so every pass
// reading `ConfigFiles` sees the same "compiled" methods without a real
// baseline-profile file or agg_method_stats CSV.
void mark_compiled(ConfigFiles& conf, const std::vector<DexMethod*>& methods) {
  method_profiles::Stats hot_stats;
  hot_stats.appear_percent = 100.0;
  hot_stats.call_count = 100.0;
  UnorderedMap<const DexMethodRef*, method_profiles::Stats> manual_hot_stats;
  for (auto* m : methods) {
    manual_hot_stats[m] = hot_stats;
  }
  conf.get_method_profiles() = method_profiles::MethodProfiles::initialize(
      "manual_hot", manual_hot_stats);
}

// A NON-FINAL virtual callee, in a non-final class, that no other method
// anywhere in the fixture overrides -- a "non-true-virtual" -- reached from
// one hot and two cold call sites via `invoke-virtual`. Otherwise identical
// in shape to `MixedHotnessFixture`.
struct NonFinalVirtualMixedHotnessFixture {
  DexMethod* callee;
  DexMethod* hot_caller;
  DexMethod* cold_caller1;
  DexMethod* cold_caller2;
  std::vector<DexStore> stores;
};

NonFinalVirtualMixedHotnessFixture
make_non_final_virtual_mixed_hotness_fixture() {
  ClassCreator cc{DexType::make_type("LCnicE2eVirtual;")};
  cc.set_super(type::java_lang_Object());

  std::ostringstream callee_code;
  callee_code << "(\n  (load-param-object v0)\n"
                 "  (.src_block \"LCnicE2eVirtual;.callee:()V\" 0 (0.7 60))\n"
                 "  (const v1 1)\n";
  for (size_t i = 0; i < 14; i++) {
    callee_code << "  (add-int v1 v1 v1)\n";
  }
  callee_code << "  (return-void)\n)";
  auto* callee =
      DexMethod::make_method("LCnicE2eVirtual;.callee:()V")
          ->make_concrete(ACC_PUBLIC,
                          assembler::ircode_from_string(callee_code.str()),
                          /* is_virtual */ true);
  callee->set_deobfuscated_name(show(callee));
  callee->get_code()->build_cfg();
  cc.add_method(callee);

  auto make_caller = [&](const std::string& name, const std::string& sb) {
    auto* m =
        DexMethod::make_method("LCnicE2eVirtual;." + name +
                               ":(LCnicE2eVirtual;)V")
            ->make_concrete(
                ACC_PUBLIC | ACC_STATIC,
                assembler::ircode_from_string(
                    "(\n  (load-param-object v0)\n  (.src_block \"" +
                    std::string("LCnicE2eVirtual;.") + name +
                    ":(LCnicE2eVirtual;)V\" 0 " + sb +
                    ")\n"
                    "  (invoke-virtual (v0) \"LCnicE2eVirtual;.callee:()V\")\n"
                    "  (return-void)\n)"),
                false);
    m->set_deobfuscated_name(show(m));
    m->get_code()->build_cfg();
    cc.add_method(m);
    return m;
  };
  auto* hot_caller = make_caller("hot_caller", "(1.0 100)");
  auto* cold_caller1 = make_caller("cold_caller1", "(0 0)");
  auto* cold_caller2 = make_caller("cold_caller2", "(0 0)");

  DexStore store("classes");
  store.add_classes({cc.create()});
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(store));

  return NonFinalVirtualMixedHotnessFixture{callee, hot_caller, cold_caller1,
                                            cold_caller2, std::move(stores)};
}

} // namespace

class CallsiteNeverInlineCloningEndToEndTest : public RedexTest {};

TEST_F(CallsiteNeverInlineCloningEndToEndTest,
       ArtProfileWriterAnnotatesOnlyTheClone) {
  auto fx = make_mixed_hotness_fixture();

  // A permissive CallsiteNeverInlineCloningPass config (mirrors this pass's
  // own test suite's `test_config()`), and
  // `never_inline_attach_annotations=true` on ArtProfileWriterPass, which
  // defaults to false (a dry classification run that never touches
  // bytecode).
  Json::Value json_conf;
  json_conf["CallsiteNeverInlineCloningPass"]["hot_block_appear_threshold"] =
      80.0;
  json_conf["CallsiteNeverInlineCloningPass"]["hot_method_appear_threshold"] =
      20.0;
  json_conf["CallsiteNeverInlineCloningPass"]["min_hot_callsites"] = 1;
  json_conf["CallsiteNeverInlineCloningPass"]["min_cold_callsites"] = 2;
  json_conf["CallsiteNeverInlineCloningPass"]
           ["min_estimated_oat_code_units_saved"] = 0;
  json_conf["CallsiteNeverInlineCloningPass"]["method_ref_cost"] = 0;
  json_conf["ArtProfileWriterPass"]["never_inline_attach_annotations"] = true;

  ConfigFiles conf(json_conf);
  conf.parse_global_config();
  auto tmp_dir = redex::make_tmp_dir("cnic_e2e_%%%%%%%%");
  conf.set_outdir(tmp_dir.path);
  mark_compiled(conf,
                {fx.callee, fx.hot_caller, fx.cold_caller1, fx.cold_caller2});

  CallsiteNeverInlineCloningPass cnic_pass;
  ArtProfileWriterPass apw_pass;
  PassManager manager({&cnic_pass, &apw_pass}, conf, RedexOptions{});
  manager.run_passes(fx.stores, conf);

  auto* never_inline_anno = type::dalvik_annotation_optimization_NeverInline();

  // The hot caller still calls the original, and the original was never
  // annotated: it still has a call site ArtProfileWriterPass could not
  // prove cold (`hot_hot_callees`, in that pass's own terms), so its own
  // never_inline() analysis skips it -- not because this test asserts
  // shape-eligibility a second time, but because that is genuinely what the
  // unmodified pass decided.
  EXPECT_EQ(find_invoked_method(fx.hot_caller), fx.callee);
  EXPECT_FALSE(has_anno(fx.callee, never_inline_anno));

  // Both cold callers now call a clone -- and ArtProfileWriterPass, seeing
  // that clone reached only from calls it cannot prove hot, actually
  // attached `@NeverInline` to it.
  auto* clone = find_invoked_method(fx.cold_caller1);
  ASSERT_NE(clone, nullptr);
  EXPECT_NE(clone, fx.callee);
  EXPECT_EQ(find_invoked_method(fx.cold_caller2), clone);
  EXPECT_TRUE(has_anno(clone, never_inline_anno));
}

TEST_F(CallsiteNeverInlineCloningEndToEndTest,
       ArtProfileWriterAloneNeverAnnotatesTheMixedHotnessOriginal) {
  // The negative control: without CallsiteNeverInlineCloningPass running
  // first, ArtProfileWriterPass on its own -- unmodified, same
  // `never_inline_attach_annotations=true` -- refuses to annotate a callee
  // that still has a call site it cannot prove cold. This is the exact
  // conflict CallsiteNeverInlineCloningPass exists to resolve; this test
  // proves ArtProfileWriterPass really does have that veto on its own, so
  // the other test's "unannotated original" outcome is not a vacuous
  // assertion.
  auto fx = make_mixed_hotness_fixture();

  Json::Value json_conf;
  json_conf["ArtProfileWriterPass"]["never_inline_attach_annotations"] = true;

  ConfigFiles conf(json_conf);
  conf.parse_global_config();
  auto tmp_dir = redex::make_tmp_dir("cnic_e2e_%%%%%%%%");
  conf.set_outdir(tmp_dir.path);
  mark_compiled(conf,
                {fx.callee, fx.hot_caller, fx.cold_caller1, fx.cold_caller2});

  ArtProfileWriterPass apw_pass;
  PassManager manager({&apw_pass}, conf, RedexOptions{});
  manager.run_passes(fx.stores, conf);

  auto* never_inline_anno = type::dalvik_annotation_optimization_NeverInline();
  EXPECT_FALSE(has_anno(fx.callee, never_inline_anno));
  EXPECT_EQ(find_invoked_method(fx.hot_caller), fx.callee);
  EXPECT_EQ(find_invoked_method(fx.cold_caller1), fx.callee);
  EXPECT_EQ(find_invoked_method(fx.cold_caller2), fx.callee);
}

TEST_F(CallsiteNeverInlineCloningEndToEndTest,
       NonFinalNonTrueVirtualCloneIsMarkedFinalAndAnnotated) {
  // A non-final virtual callee that no other method overrides (a
  // "non-true-virtual") is only eligible for cloning at all because of
  // CallsiteNeverInlineCloningPass's own MethodOverrideGraph-based
  // eligibility check -- but its clone, left at the original's own
  // non-final access flags, would itself be invisible to
  // ArtProfileWriterPass's narrower `is_final(method) || is_final(cls)`
  // virtual-eligibility check. This proves the fix holds end to end: the
  // clone is marked `final` (collision-checked first), stays virtual,
  // public, and reachable via the exact same `invoke-virtual` opcode its
  // callers already used, and ArtProfileWriterPass genuinely annotates it.
  auto fx = make_non_final_virtual_mixed_hotness_fixture();
  ASSERT_FALSE(is_final(fx.callee));
  ASSERT_FALSE(is_final(type_class(fx.callee->get_class())));

  Json::Value json_conf;
  json_conf["CallsiteNeverInlineCloningPass"]["hot_block_appear_threshold"] =
      80.0;
  json_conf["CallsiteNeverInlineCloningPass"]["hot_method_appear_threshold"] =
      20.0;
  json_conf["CallsiteNeverInlineCloningPass"]["min_hot_callsites"] = 1;
  json_conf["CallsiteNeverInlineCloningPass"]["min_cold_callsites"] = 2;
  json_conf["CallsiteNeverInlineCloningPass"]
           ["min_estimated_oat_code_units_saved"] = 0;
  json_conf["CallsiteNeverInlineCloningPass"]["method_ref_cost"] = 0;
  json_conf["ArtProfileWriterPass"]["never_inline_attach_annotations"] = true;

  ConfigFiles conf(json_conf);
  conf.parse_global_config();
  auto tmp_dir = redex::make_tmp_dir("cnic_e2e_%%%%%%%%");
  conf.set_outdir(tmp_dir.path);
  mark_compiled(conf,
                {fx.callee, fx.hot_caller, fx.cold_caller1, fx.cold_caller2});

  CallsiteNeverInlineCloningPass cnic_pass;
  ArtProfileWriterPass apw_pass;
  PassManager manager({&cnic_pass, &apw_pass}, conf, RedexOptions{});
  manager.run_passes(fx.stores, conf);

  auto* never_inline_anno = type::dalvik_annotation_optimization_NeverInline();

  EXPECT_EQ(find_invoked_method(fx.hot_caller), fx.callee);
  EXPECT_FALSE(has_anno(fx.callee, never_inline_anno));
  EXPECT_FALSE(is_final(fx.callee));

  auto* clone = find_invoked_method(fx.cold_caller1);
  ASSERT_NE(clone, nullptr);
  EXPECT_NE(clone, fx.callee);
  EXPECT_EQ(find_invoked_method(fx.cold_caller2), clone);
  // Preserved unchanged from the original.
  EXPECT_TRUE(clone->is_virtual());
  EXPECT_TRUE(is_public(clone));
  // The one access-flag change this pass makes for exactly this category of
  // candidate.
  EXPECT_TRUE(is_final(clone));
  EXPECT_TRUE(has_anno(clone, never_inline_anno));
}
