/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <set>

#include "DexPosition.h"
#include "RedexContext.h"
#include "RedexTest.h"

namespace {

// Interning is private, so these drive it through `make_pattern`, which interns
// every position it is handed.
class PositionInterningTest : public RedexTest {
 protected:
  static PositionPatternSwitchManager* manager() {
    return g_redex->get_position_pattern_switch_manager();
  }

  static std::unique_ptr<DexPosition> pos(const char* file, uint32_t line) {
    return std::make_unique<DexPosition>(DexString::make_string("LFoo;.m:()V"),
                                         DexString::make_string(file), line);
  }
};

// Interning under churn: the caller's position dies on every iteration, so the
// allocator hands the next one the same address. Each distinct line must still
// get its own pattern -- asserting that, rather than merely not crashing, is
// what makes this catch an address-keyed memo.
TEST_F(PositionInterningTest, interning_many_short_lived_positions_is_safe) {
  std::set<uint32_t> pattern_ids;
  for (uint32_t line = 1; line <= 4096; ++line) {
    auto transient = pos("Foo.java", line);
    pattern_ids.insert(manager()->make_pattern({transient.get()}));
  }
  EXPECT_EQ(pattern_ids.size(), 4096u);
}

// Interning hands back a copy the manager owns. Returning the caller's object
// would hand out a pointer that dies with the method being processed.
TEST_F(PositionInterningTest, the_interned_position_is_a_copy) {
  auto original = pos("Foo.java", 12);
  auto pattern_id = manager()->make_pattern({original.get()});

  const auto& pattern = manager()->get_patterns().at(pattern_id);
  ASSERT_EQ(pattern.size(), 1u);
  EXPECT_NE(pattern[0], original.get());
  EXPECT_EQ(*pattern[0], *original);
}

// The copy is deep: a parent chain interned by reference would still point at
// the caller's memory one level up.
TEST_F(PositionInterningTest, the_parent_chain_is_copied_too) {
  auto parent = pos("Caller.java", 1);
  auto child = pos("Foo.java", 12);
  child->parent = parent.get();

  auto pattern_id = manager()->make_pattern({child.get()});
  const auto& pattern = manager()->get_patterns().at(pattern_id);
  ASSERT_EQ(pattern.size(), 1u);
  ASSERT_NE(pattern[0]->parent, nullptr);
  EXPECT_NE(pattern[0]->parent, parent.get());
  EXPECT_EQ(*pattern[0]->parent, *parent);
}

// The regression this exists for, and it fails on the code before the fix.
// The table is keyed by `DexPosition*` with pointer equality, but was hashed by
// the pointed-to value. A lookup hashed the caller's position into the right
// bucket and then compared pointers against the stored key -- which is the
// manager's own clone, never the caller's object. Nothing ever matched, so
// interning cloned again on every single call: the table grew with the number
// of calls rather than the number of distinct positions.
TEST_F(PositionInterningTest, the_same_object_interns_once) {
  auto p = pos("Foo.java", 12);
  EXPECT_EQ(manager()->make_pattern({p.get()}),
            manager()->make_pattern({p.get()}));
}

// Interning means one object per distinct source location, not one per call.
// Two live objects never share an address, so an address-keyed memo cannot
// deliver this no matter how it is written.
TEST_F(PositionInterningTest, equal_values_intern_to_one_object) {
  auto a = pos("Foo.java", 12);
  auto b = pos("Foo.java", 12);
  ASSERT_NE(a.get(), b.get());
  EXPECT_EQ(manager()->make_pattern({a.get()}),
            manager()->make_pattern({b.get()}));
}

// Keying the memo on the caller's address is unsound because the address
// outlives what it names: `a` is freed, `b` is allocated on top of it, and `b`
// inherits `a`'s clone -- reporting Foo.java:2 as line 1. Address reuse is not
// guaranteed by the standard, but it is what the allocator does here, and this
// test was observed failing on the address-keyed version.
TEST_F(PositionInterningTest, a_recycled_address_does_not_alias) {
  uint32_t first;
  {
    auto a = pos("Foo.java", 1);
    first = manager()->make_pattern({a.get()});
  }
  auto b = pos("Foo.java", 2);
  auto second = manager()->make_pattern({b.get()});

  EXPECT_NE(first, second);
  ASSERT_EQ(manager()->get_patterns().at(second).size(), 1u);
  EXPECT_EQ(manager()->get_patterns().at(second)[0]->line, 2u);
}

// The counters behind the `num_internalize_calls` / `num_interned_positions`
// Redex metrics: three calls naming two distinct locations keep two positions.
TEST_F(PositionInterningTest, interning_counters_report_the_saving) {
  auto a = pos("Foo.java", 12);
  auto b = pos("Foo.java", 12);
  auto c = pos("Foo.java", 13);
  manager()->make_pattern({a.get()});
  manager()->make_pattern({b.get()});
  manager()->make_pattern({c.get()});

  EXPECT_EQ(manager()->get_num_internalize_calls(), 3u);
  EXPECT_EQ(manager()->get_num_interned_positions(), 2u);
}

} // namespace
