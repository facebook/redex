/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include <json/value.h>

#include "ABExperimentContext.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "PassManager.h"
#include "ProguardConfiguration.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "RedexContext.h"
#ifndef IS_REDEX_TEST_LIBRARY
#include "SanitizersConfig.h"
#endif
#include "RedexTestUtils.h"

struct RedexTest : public testing::Test {
  RedexTest() { g_redex = new RedexContext(); }

  ~RedexTest() { delete g_redex; }

  void reset_ab_experiments_global_state() {
    ab_test::ABExperimentContext::reset_global_state();
  }
};

struct RedexIntegrationTest : public RedexTest {
 protected:
  const char* dex_file;
  const char* secondary_dex_file;
  std::vector<DexStore> stores;
  boost::optional<DexClasses&> classes;
  DexMetadata dex_metadata;
  redex::TempDir configfiles_out_dir;
  std::unique_ptr<ConfigFiles> conf;
  std::unique_ptr<PassManager> pass_manager;

 public:
  RedexIntegrationTest() {
    dex_file = std::getenv("dexfile");

    always_assert_log(dex_file,
                      "Dex file must be set up before integration tests.\n");

    secondary_dex_file = std::getenv("secondary_dexfile");

    dex_metadata.set_id("classes");
    DexStore root_store(dex_metadata);
    root_store.add_classes(
        load_classes_from_dex(DexLocation::make_location("dex", dex_file)));
    if (secondary_dex_file) {
      root_store.add_classes(load_classes_from_dex(
          DexLocation::make_location("dex", secondary_dex_file)));
    }
    classes = root_store.get_dexen().back();
    stores.emplace_back(std::move(root_store));
    configfiles_out_dir = redex::make_tmp_dir("RedexIntegrationTest%%%%%%%%");
  }

  std::string& get_configfiles_out_dir() { return configfiles_out_dir.path; }

  // NOTE: The defaults for RedexOptions are technically bad, as the
  //       PassManager survives the `run_passes` call, at which point
  //       the options object has gone out of scope. But simplicity...

  void run_passes(
      const std::vector<Pass*>& passes,
      std::unique_ptr<keep_rules::ProguardConfiguration> pg_config = nullptr,
      const Json::Value& json_conf = Json::nullValue,
      const RedexOptions& redex_options = RedexOptions{}) {
    run_passes(
        passes, std::move(pg_config), json_conf, [](const auto&) {},
        [](const auto&) {}, redex_options);
  }

  template <typename MgrFn>
  void run_passes(const std::vector<Pass*>& passes,
                  std::unique_ptr<keep_rules::ProguardConfiguration> pg_config,
                  const Json::Value& json_conf,
                  const MgrFn& mgr_fn,
                  const RedexOptions& redex_options = RedexOptions{}) {
    run_passes(
        passes, std::move(pg_config), json_conf, [](const auto&) {}, mgr_fn,
        redex_options);
  }

  template <typename ConfFn, typename MgrFn>
  void run_passes(const std::vector<Pass*>& passes,
                  std::unique_ptr<keep_rules::ProguardConfiguration> pg_config,
                  const Json::Value& json_conf,
                  const ConfFn& conf_fn,
                  const MgrFn& mgr_fn,
                  const RedexOptions& redex_options = RedexOptions{}) {
    conf = std::make_unique<ConfigFiles>(json_conf);
    conf->parse_global_config();

    conf_fn(*conf);

    if (pg_config) {
      pass_manager = std::make_unique<PassManager>(passes, std::move(pg_config),
                                                   *conf, redex_options);
    } else {
      pass_manager =
          std::make_unique<PassManager>(passes, *conf, redex_options);
    }

    mgr_fn(*pass_manager);

    pass_manager->set_testing_mode();
    conf->set_outdir(configfiles_out_dir.path);
    pass_manager->run_passes(stores, *conf);
  }

  virtual ~RedexIntegrationTest() {}

  template <typename C>
  DexClass* find_class(const C& clazzes, const std::string& name) {
    const auto it = std::find_if(
        clazzes.begin(), clazzes.end(),
        [&name](const DexClass* cls) { return cls->str() == name; });
    return it == clazzes.end() ? nullptr : *it;
  }

  template <typename C>
  DexField* find_ifield(const C& clazzes,
                        const char* cls,
                        const char* type,
                        const char* name) {
    const auto* c = find_class(clazzes, cls);
    const auto& ifields = c->get_ifields();
    const auto it = std::find(ifields.begin(), ifields.end(),
                              DexField::make_field(DexType::make_type(cls),
                                                   DexString::make_string(name),
                                                   DexType::make_type(type)));
    return it == ifields.end() ? nullptr : *it;
  }

  template <typename C>
  DexMethod* find_vmethod(const C& clazzes,
                          const char* cls,
                          const char* rtype,
                          const char* name,
                          const std::vector<const char*>& args) {
    const auto* c = find_class(clazzes, cls);
    const auto& vmethods = c->get_vmethods();
    const auto it = std::find(vmethods.begin(), vmethods.end(),
                              DexMethod::make_method(cls, name, rtype, args));
    return it == vmethods.end() ? nullptr : *it;
  }

  std::unique_ptr<keep_rules::ProguardConfiguration>
  process_and_get_proguard_config(const std::vector<DexClasses>& dexen,
                                  const std::string& config) {
    auto pg_config = std::make_unique<keep_rules::ProguardConfiguration>();
    std::istringstream pg_config_text(config);
    keep_rules::proguard_parser::parse(pg_config_text, pg_config.get());

    ProguardMap pm;
    // We aren't loading any external jars for this test
    // so external_classes is empty
    Scope external_classes;
    apply_deobfuscated_names(dexen, pm);
    Scope scope = build_class_scope(dexen);
    process_proguard_rules(pm, scope, external_classes, *pg_config, true);
    return pg_config;
  }
};

/*
 * We compare IRCode objects by serializing them first. However, the serialized
 * forms lack newlines between instructions and so are rather difficult to read.
 * It's nice to print the original IRCode objects which have those newlines.
 *
 * This is a macro instead of a function so that the error messages will contain
 * the right line numbers.
 */
#define EXPECT_CODE_EQ(a, b)                                          \
  do {                                                                \
    IRCode* aCode = a;                                                \
    IRCode* bCode = b;                                                \
    std::string aStr = assembler::to_string(aCode);                   \
    std::string bStr = assembler::to_string(bCode);                   \
    if (aStr == bStr) {                                               \
      SUCCEED();                                                      \
    } else {                                                          \
      auto p = std::mismatch(aStr.begin(), aStr.end(), bStr.begin()); \
      FAIL() << '\n'                                                  \
             << "S-expressions failed to match: \n"                   \
             << aStr << '\n'                                          \
             << bStr << '\n'                                          \
             << std::string(p.first - aStr.begin(), '.') + "^\n"      \
             << "\nExpected:\n"                                       \
             << show(aCode) << "\nto be equal to:\n"                  \
             << show(bCode);                                          \
    }                                                                 \
  } while (0);
