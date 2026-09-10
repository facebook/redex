/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ChromeTraceWriter.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>
#include <json/reader.h>
#include <json/value.h>

namespace {

Json::Value read_json_file(const std::string& path) {
  std::ifstream f(path);
  Json::Value root;
  Json::Reader reader;
  EXPECT_TRUE(reader.parse(f, root));
  return root;
}

std::string make_temp_file() {
  std::string tmpl = "/tmp/chrome_trace_test_XXXXXX";
  int fd = mkstemp(tmpl.data());
  EXPECT_NE(-1, fd);
  close(fd);
  return tmpl;
}

} // namespace

TEST(ChromeTraceWriterTest, DisabledByDefault) {
  EXPECT_FALSE(ChromeTraceWriter::enabled());
}

TEST(ChromeTraceWriterTest, EnabledAfterInit) {
  ChromeTraceWriter::init();
  EXPECT_TRUE(ChromeTraceWriter::enabled());
}

TEST(ChromeTraceWriterTest, WriteProducesValidJson) {
  ChromeTraceWriter::init();

  auto start = ChromeTraceWriter::clock::now();
  auto end = start + std::chrono::microseconds(500);

  ChromeTraceWriter::record("TestTimer", start, end,
                            std::this_thread::get_id());
  ChromeTraceWriter::record("AnotherTimer", start, end,
                            std::this_thread::get_id());

  std::string path = make_temp_file();
  ChromeTraceWriter::write(path);

  Json::Value root = read_json_file(path);
  ASSERT_TRUE(root.isArray());
  // At least the 2 events we just recorded (prior tests may have added more
  // since the static event list persists across tests in the same binary).
  ASSERT_GE(root.size(), 2u);

  // Check that the last two events are ours.
  const auto& e1 = root[root.size() - 2];
  const auto& e2 = root[root.size() - 1];

  EXPECT_EQ("TestTimer", e1["name"].asString());
  EXPECT_EQ("X", e1["ph"].asString());
  EXPECT_TRUE(e1.isMember("ts"));
  EXPECT_EQ(500u, e1["dur"].asUInt64());
  EXPECT_EQ(1, e1["pid"].asInt());
  EXPECT_TRUE(e1.isMember("tid"));

  EXPECT_EQ("AnotherTimer", e2["name"].asString());
  EXPECT_EQ("X", e2["ph"].asString());
  EXPECT_EQ(500u, e2["dur"].asUInt64());

  std::remove(path.c_str());
}
