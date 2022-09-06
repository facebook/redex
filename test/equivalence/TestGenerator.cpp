/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_set>

#include <json/value.h>

#include "DexLoader.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTestUtils.h"
#include "SanitizersConfig.h"
#include "TestGenerator.h"

namespace fs = boost::filesystem;

void EquivalenceTest::generate(DexClass* cls) {
  setup(cls);
  auto ret = DexType::make_type("I");
  auto args = DexTypeList::make_type_list({});
  auto proto = DexProto::make_proto(ret, args); // I()
  DexMethod* before = static_cast<DexMethod*>(DexMethod::make_method(
      cls->get_type(), DexString::make_string("before_" + test_name()), proto));
  before->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  before->set_code(std::make_unique<IRCode>(before, 0));
  build_method(before);
  cls->add_method(before);
  auto after = DexMethod::make_method_from(
      before, cls->get_type(), DexString::make_string("after_" + test_name()));
  cls->add_method(after);
  transform_method(after);
}

void EquivalenceTest::generate_all(DexClass* cls) {
  std::unordered_set<std::string> test_names;
  for (auto& test : all_tests()) {
    always_assert(!test_names.count(test->test_name()));
    test_names.insert(test->test_name());
    test->generate(cls);
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: ./TestGenerator classes.dex" << std::endl;
  }
  auto dex = argv[1];

  g_redex = new RedexContext();
  auto classes = load_classes_from_dex(DexLocation::make_location("", dex));
  auto runner_cls =
      std::find_if(classes.begin(), classes.end(), [](const DexClass* cls) {
        return cls->get_name() ==
               DexString::get_string(
                   "Lcom/facebook/redex/equivalence/EquivalenceMain;");
      });

  redex_assert(runner_cls != classes.end());

  EquivalenceTest::generate_all(*runner_cls);

  Json::Value json(Json::objectValue);
  auto tmpdir = redex::make_tmp_dir("redex_equivalence_test_%%%%%%%%");
  std::string metadir(tmpdir.path);
  metadir += "/meta";
  int status = mkdir(metadir.c_str(), 0755);
  if (status != 0) {
    // Attention: errno may get changed by syscalls or lib functions.
    // Saving before printing is a conventional way of using errno.
    int errsv = errno;
    std::cerr << "error: cannot mkdir meta in outdir. errno = " << errsv
              << std::endl;
    exit(EXIT_FAILURE);
  }

  ConfigFiles conf(json, tmpdir.path);
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));

  DexStore store("classes");
  store.set_dex_magic(
      load_dex_magic_from_dex(DexLocation::make_location("dex", dex)));
  store.add_classes(classes);
  DexStoresVector stores;
  stores.emplace_back(std::move(store));
  instruction_lowering::run(stores);

  RedexOptions redex_options;
  auto gtypes = std::make_shared<GatheredTypes>(&classes);

  write_classes_to_dex(redex_options,
                       dex,
                       &classes,
                       std::move(gtypes),
                       nullptr /* LocatorIndex* */,
                       0,
                       nullptr /* store_name */,
                       0,
                       conf,
                       pos_mapper.get(),
                       nullptr,
                       nullptr,
                       nullptr /* IODIMetadata* */,
                       stores[0].get_dex_magic());

  delete g_redex;
  return 0;
}
