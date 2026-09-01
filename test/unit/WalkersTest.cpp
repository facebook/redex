/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Walkers.h"

#include <gmock/gmock.h>

#include "Creators.h"
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
  EXPECT_THAT(strings,
              ::testing::UnorderedElementsAre("LFoo;.bar:()V", "LFoo;.baz:()V",
                                              "LFoo;.qux:()V",
                                              "LFoo;.quux:()V"));
}

// A non-identity `init` must be folded in exactly once, whatever the thread
// count. Seeding the per-thread slots with `init` instead of the identity
// folds it once per thread plus once at the end, which makes the result vary
// with the host's core count.
TEST_F(WalkersTest, accumulate_with_init_is_thread_count_independent) {
  ClassCreator cc(DexType::make_type("LInit;"));
  cc.set_super(type::java_lang_Object());
  cc.add_method(DexMethod::make_method("LInit;.a:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));
  cc.add_method(DexMethod::make_method("LInit;.b:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));
  cc.add_method(DexMethod::make_method("LInit;.c:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false));

  Scope scope{cc.create()};
  constexpr size_t kInit = 100;
  for (size_t num_threads : {size_t(1), size_t(2), size_t(8)}) {
    auto sum = walk::parallel::methods<size_t>(
        scope, [](DexMethod*) { return size_t(1); }, num_threads, kInit);
    EXPECT_EQ(sum, kInit + 3) << "methods, " << num_threads << " threads";

    // The `classes` overload seeds its slots the same way.
    auto class_sum = walk::parallel::classes<size_t>(
        scope, [](DexClass*) { return size_t(1); }, num_threads, kInit);
    EXPECT_EQ(class_sum, kInit + 1) << "classes, " << num_threads << " threads";

    // A non-arithmetic Accumulator with a non-trivial identity: the empty set.
    using StringSet = std::unordered_set<std::string>;
    StringSet seed{"seed"};
    auto merged =
        walk::parallel::methods<StringSet, MergeContainers<StringSet>>(
            scope, [](DexMethod* m) { return StringSet{show(m)}; }, num_threads,
            seed);
    EXPECT_EQ(merged.size(), 4u) << "merge, " << num_threads << " threads";
    EXPECT_EQ(merged.count("seed"), 1u);
  }
}
