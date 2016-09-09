/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

#include <json/json.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "RedexContext.h"

#include "RemoveUnreachable.h"

void write_seeds_file(
  std::string filename,
  const std::vector<std::string>& seeds
) {
  std::ofstream out(filename);
  for (auto const& seed : seeds) {
    out << seed << "\n";
  }
}

void init_test_seeds(const ProguardMap& pm) {
  std::vector<std::string> seeds = {
    "A",
    "A: int foo",
    "A: A()",
    "A: int bar()",
    "I: void wat()",
    "BadgerTester: boolean testBadger(Badger)",
    "HogBadger",
    "UseIt: void go(IChild)",
    "UseHasher: void test()",
  };
  char tmpfilename[] = "/tmp/seeds.XXXXXX";
  int fd = mkstemp(tmpfilename);
  write_seeds_file(tmpfilename, seeds);
  auto nseeds = init_seed_classes(tmpfilename, pm);
  unlink(tmpfilename);
  close(fd);
  ASSERT_EQ(seeds.size(), nseeds);
}

template<typename C>
DexClass* find_class(const C& classes, const char* name) {
  auto const it = std::find_if(
    classes.begin(), classes.end(),
    [&name](const DexClass* cls) {
      return !strcmp(cls->c_str(), name);
    });
  return it == classes.end() ? nullptr : *it;
}

template<typename C>
DexField* find_ifield(
  const C& classes,
  const char* cls,
  const char* type,
  const char* name
) {
  auto const& c = find_class(classes, cls);
  auto const& ifields = c->get_ifields();
  auto const it = std::find(
    ifields.begin(), ifields.end(),
    DexField::make_field(
      DexType::make_type(cls),
      DexString::make_string(name),
      DexType::make_type(type)));
  return it == ifields.end() ? nullptr : *it;
}

template<typename C>
DexMethod* find_vmethod(
  const C& classes,
  const char* cls,
  const char* rtype,
  const char* name,
  const std::vector<const char*>& args
) {
  auto const& c = find_class(classes, cls);
  auto const& vmethods = c->get_vmethods();
  auto const it = std::find(
    vmethods.begin(), vmethods.end(),
    DexMethod::make_method(cls, name, rtype, args));
  return it == vmethods.end() ? nullptr : *it;
}

TEST(RemoveUnreachableTest, synthetic) {
  g_redex = new RedexContext();

  // Hardcoded path is for OSS automake test harness, environment variable is
  // for Buck
  const char* dexfile = "remove-unreachable-test.dex";
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);
  }

  std::vector<DexStore> stores;
  DexStore root_store("classes");
  root_store.add_classes(load_classes_from_dex(dexfile));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));

  std::vector<Pass*> passes = {
    new RemoveUnreachablePass(),
  };

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);

  init_test_seeds(dummy_cfg.get_proguard_map());

  // Make sure some unreachable things exist before we start.
  ASSERT_TRUE(find_vmethod(classes, "LA;", "V", "bor", {}));

  std::vector<KeepRule> null_rules;
  PassManager manager(passes, null_rules);
  manager.run_passes(stores, dummy_cfg);

  // Seed elements
  ASSERT_TRUE(find_class(classes, "LA;"));
  ASSERT_TRUE(find_ifield(classes, "LA;", "I", "foo"));
  ASSERT_TRUE(find_vmethod(classes, "LA;", "I", "bar", {}));

  // Elements transitively reachable via seeds.
  ASSERT_TRUE(find_vmethod(classes, "LA;", "I", "baz", {}));

  // Overrides of reachable elements
  ASSERT_TRUE(find_class(classes, "LD;"));
  ASSERT_TRUE(find_vmethod(classes, "LD;", "I", "bar", {}));
  ASSERT_TRUE(find_vmethod(classes, "LD;", "I", "baz", {}));

  // Weird inheritance triangle thingie.
  // I.wat() is kept
  // Sub implements I
  // Sub extends Super
  // Sub does *not* define wat(), but Super does
  // Super.wat() is a thing that must be kept.
  ASSERT_TRUE(find_class(classes, "LI;"));
  ASSERT_TRUE(find_class(classes, "LSuper;"));
  ASSERT_TRUE(find_vmethod(classes, "LI;", "V", "wat", {}));
  ASSERT_TRUE(find_vmethod(classes, "LSuper;", "V", "wat", {}));

  // Another tricky inheritance case.
  ASSERT_TRUE(find_class(classes, "LHoneyBadger;"));
  ASSERT_TRUE(find_vmethod(classes, "LHoneyBadger;", "Z", "isAwesome", {}));
  // You might think that HogBadger.isAwesome() can be removed, since it
  // doesn't extend Badger.  But it's very tricky to remove this while still
  // getting the Guava Hasher case (below) correct.
  ASSERT_TRUE(find_vmethod(classes, "LHogBadger;", "Z", "isAwesome", {}));

  // Still more inheritance trickiness.
  ASSERT_TRUE(find_class(classes, "LIParent;"));
  ASSERT_TRUE(find_class(classes, "LIChild;"));
  ASSERT_TRUE(find_vmethod(classes, "LIParent;", "V", "go", {}));
  ASSERT_FALSE(find_vmethod(classes, "LIChild;", "V", "go", {}));

  // Inheritance case from Guava
  ASSERT_TRUE(find_class(classes, "LHasher;"));
  ASSERT_TRUE(find_class(classes, "LAbstractHasher;"));
  ASSERT_TRUE(find_class(classes, "LTestHasher;"));
  ASSERT_TRUE(find_vmethod(classes, "LHasher;", "V", "putBytes", {}));
  ASSERT_TRUE(find_vmethod(classes, "LTestHasher;", "V", "putBytes", {}));

  // Elements not reachable via seeds.
  ASSERT_FALSE(find_vmethod(classes, "LA;", "V", "bor", {}));

  // Override of nonreachable elements.
  ASSERT_FALSE(find_vmethod(classes, "LD;", "V", "bor", {}));

  delete g_redex;
}
