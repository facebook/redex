/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/regex.hpp>
#include <gtest/gtest.h>
#include <map>

#include "ApkResources.h"
#include "OptimizeResources.h"
#include "RedexResources.h"
#include "androidfw/ResourceTypes.h"
#include "verify/VerifyUtil.h"

namespace {
void assert_string_matches(const std::string& actual,
                           const char* expected_pattern) {
  boost::regex reg(expected_pattern);
  ASSERT_TRUE(boost::regex_match(actual, reg))
      << actual << " did not match pattern " << expected_pattern;
}
} // namespace

void postverify_impl(const DexClasses& /* unused */,
                     const std::function<std::vector<std::string>(uint32_t)>&
                         string_value_getter,
                     ResourceTableFile* res_table) {
  // The original dimen type started out at index 2, and the table had 6 total
  // types. Verify the following:
  // 1) New type was defined at index 7.
  // 2) IDs with default only values are moved to type 7, in sequential order.
  // 3) The original dimen table is compacted, with sequential IDs, except when
  //    holes were given in the input file.
  const auto old_dimen_mask = 0x7f02;
  std::map<uint32_t, uint32_t> expected_counts;
  expected_counts.emplace(old_dimen_mask, 12);
  expected_counts.emplace(0x7f08, 91); // dimen2 type
  expected_counts.emplace(0x7f06, 1); // original plurals type
  expected_counts.emplace(0x7f09, 4); // drawable split for hdpi
  expected_counts.emplace(0x7f0a, 3); // drawable split for xhdpi
  expected_counts.emplace(0x7f09, 5); // plurals2 type

  auto id_count = res_table->sorted_res_ids.size();
  ASSERT_EQ(id_count, 124);

  std::map<uint32_t, uint32_t> actual_counts;
  uint32_t max_dimen_id = 0;
  for (size_t i = 0; i < id_count; i++) {
    auto id = res_table->sorted_res_ids[i];
    auto upper = id >> TYPE_INDEX_BIT_SHIFT;
    actual_counts[upper]++;
    if (upper == old_dimen_mask) {
      max_dimen_id = id;
    }
  }

  for (const auto& t : expected_counts) {
    ASSERT_EQ(t.second, actual_counts[t.first]);
  }

  // See splitres_static_ids for "deleted" items.
  size_t num_holes = 0;
  for (auto i = max_dimen_id; i >= 0x7f020000; i--) {
    if (std::find(res_table->sorted_res_ids.begin(),
                  res_table->sorted_res_ids.end(),
                  i) == res_table->sorted_res_ids.end()) {
      num_holes++;
    }
  }
  ASSERT_EQ(num_holes, 3);

  // Verify old table was compacted
  ASSERT_EQ(max_dimen_id,
            (old_dimen_mask << TYPE_INDEX_BIT_SHIFT) +
                expected_counts[old_dimen_mask] + num_holes - 1);

  // Validate the drawable splits, for default values and density specific
  // values.
  {
    auto string_values = string_value_getter(0x7f090003);
    ASSERT_EQ(string_values.size(), 1);
    auto val = string_values[0];
    // Seems that aapt sometimes outputs a dummy -v4 suffix on some resources,
    // but not others. May depend on some versioning or flags, but regardless
    // make the checks here flexible for with/without so this doesn't get marked
    // as flaky. Main thing to verify is the dpi qualifiers.
    assert_string_matches(val, "res/drawable-hdpi(-v\\d+)?/d6.xml");
  }
  {
    // Should not have been moved
    auto string_values = string_value_getter(0x7f030001);
    ASSERT_EQ(string_values.size(), 2);
    assert_string_matches(string_values[0], "res/drawable(-v\\d+)?/d4.xml");
    assert_string_matches(string_values[1],
                          "res/drawable-hdpi(-v\\d+)?/d4.xml");
  }
  {
    auto string_values = string_value_getter(0x7f0a0001);
    ASSERT_EQ(string_values.size(), 1);
    assert_string_matches(string_values[0],
                          "res/drawable-xhdpi(-v\\d+)?/d11.xml");
  }
  {
    auto string_values = string_value_getter(0x7f0a0002);
    ASSERT_EQ(string_values.size(), 1);
    assert_string_matches(string_values[0],
                          "res/drawable-xhdpi(-v\\d+)?/d7.xml");
  }
  {
    // Should not have been moved
    auto string_values = string_value_getter(0x7f030002);
    ASSERT_EQ(string_values.size(), 2);
    assert_string_matches(string_values[0], "res/drawable(-v\\d+)?/d8.xml");
    assert_string_matches(string_values[1],
                          "res/drawable-xhdpi(-v\\d+)?/d8.xml");
  }

  // Make sure the pinned resource didn't get moved, even though it would
  // otherwise be elligible.
  auto a_text_size = res_table->id_to_name.at(0x7f020001);
  ASSERT_STREQ(a_text_size.c_str(), "a_text_size");
  auto not_found = res_table->id_to_name.end();
  ASSERT_EQ(res_table->id_to_name.find(0x7f020000), not_found);
  ASSERT_EQ(res_table->id_to_name.find(0x7f020002), not_found);
  ASSERT_EQ(res_table->id_to_name.find(0x7f020003), not_found);
}
