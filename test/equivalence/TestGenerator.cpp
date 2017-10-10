/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

#include <json/json.h>

#include "DexOutput.h"
#include "DexLoader.h"
#include "InstructionLowering.h"
#include "IRCode.h"
#include "TestGenerator.h"

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
  auto classes = load_classes_from_dex(dex);
  auto runner_cls =
      std::find_if(classes.begin(), classes.end(), [](const DexClass* cls) {
        return cls->get_name() ==
               DexString::get_string(
                   "Lcom/facebook/redex/equivalence/EquivalenceMain;");
      });

  assert(runner_cls != classes.end());

  EquivalenceTest::generate_all(*runner_cls);

  Json::Value json(Json::objectValue);
  ConfigFiles cfg(json);
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make("", ""));

  DexStore store("classes");
  store.add_classes(classes);
  DexStoresVector stores;
  stores.emplace_back(std::move(store));
  instruction_lowering::run(stores);

  write_classes_to_dex(dex,
                       &classes,
                       nullptr /* LocatorIndex* */,
                       0,
                       cfg,
                       json,
                       pos_mapper.get());

  delete g_redex;
  return 0;
}
