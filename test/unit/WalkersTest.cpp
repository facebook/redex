/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Walkers.h"

#include <gmock/gmock.h>

#include "DexUtil.h"
#include "RedexTest.h"
#include "Show.h"

struct WalkersTest : public RedexTest {};

TEST_F(WalkersTest, accumlate) {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(type::java_lang_Object());
  cc.add_method(DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));
  cc.add_method(DexMethod::make_method("LFoo;.baz:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));
  cc.add_method(DexMethod::make_method("LFoo;.qux:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));
  cc.add_method(DexMethod::make_method("LFoo;.quux:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));

  using StringSet = std::unordered_set<std::string>;
  constexpr size_t num_threads = 2;
  Scope scope{cc.create()};
  auto strings = walk::parallel::methods<StringSet, MergeContainers<StringSet>>(
      scope, [&](DexMethod* m) { return StringSet{show(m)}; }, num_threads);
  EXPECT_THAT(
      strings,
      ::testing::UnorderedElementsAre(
          "LFoo;.bar:()V", "LFoo;.baz:()V", "LFoo;.qux:()V", "LFoo;.quux:()V"));
}
