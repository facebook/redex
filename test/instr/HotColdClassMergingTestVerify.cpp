/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/regex.hpp>

#include "verify/VerifyUtil.h"

namespace {

constexpr const char* kPrefix =
    "Lcom/facebook/redextest/HotColdClassMergingTest$";

std::string worker_name(int i) { return kPrefix + std::to_string(i) + ";"; }

} // namespace

TEST_F(PreVerify, WorkersExist) {
  for (int i = 1; i <= 8; i++) {
    EXPECT_NE(find_class_named(classes, worker_name(i)), nullptr)
        << worker_name(i);
  }
}

// $1..$4 are hot in the method profile and $5..$8 are not, so they merge into
// two shapes: one whose work() dispatch returns only 10x constants and one
// whose dispatch returns only 20x constants.
TEST_F(PostVerify, HotAndColdWorkersMergeSeparately) {
  for (int i = 1; i <= 8; i++) {
    verify_class_merged(find_class_named(classes, worker_name(i)));
  }

  boost::regex shape_pattern("^Lcom/facebook/redex/Anon\\w*WorkerShape_\\w+;$");
  std::vector<DexClass*> shapes;
  for (auto* cls : classes) {
    if (boost::regex_match(cls->get_name()->c_str(), shape_pattern)) {
      shapes.push_back(cls);
    }
  }
  ASSERT_EQ(shapes.size(), 2u);

  boost::regex hot_const("\\(const v\\d+ 10[1-4]\\)");
  boost::regex cold_const("\\(const v\\d+ 20[1-4]\\)");
  size_t hot_shapes = 0;
  size_t cold_shapes = 0;
  for (auto* shape : shapes) {
    auto* work = find_vmethod_named(*shape, "work");
    ASSERT_NE(work, nullptr) << show(shape);
    const auto code = stringify_for_comparision(work);
    const bool has_hot = boost::regex_search(code, hot_const);
    const bool has_cold = boost::regex_search(code, cold_const);
    EXPECT_NE(has_hot, has_cold)
        << "mixed or empty dispatch in " << show(shape) << ":\n"
        << code;
    hot_shapes += has_hot ? 1 : 0;
    cold_shapes += has_cold ? 1 : 0;
  }
  EXPECT_EQ(hot_shapes, 1u);
  EXPECT_EQ(cold_shapes, 1u);
}
