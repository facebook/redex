/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "ResourceValueMergingPassVerifyImpl.h"
#include "verify/VerifyUtil.h"

TEST_F(PreVerify, BundleResourceValueMergingPassTest) {
  auto tmp_dir =
      redex::make_tmp_dir("BundleResourceValueMergingPassVerify%%%%%%%%");
  boost::filesystem::path tmp_path(tmp_dir.path);
  auto base_res_dir = tmp_path / "base";
  auto local_resources_pb = base_res_dir / "resources.pb";

  const auto& source_resource_pb = resources["base/resources.pb"];
  create_directories(base_res_dir);
  redex::copy_file(source_resource_pb, local_resources_pb.string());

  boost::filesystem::ofstream(tmp_path / "BundleConfig.pb").close();

  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(local_resources_pb.string());

  StyleAnalysis style_analysis = create_style_analysis(tmp_path, classes);
  resource_value_merging_PreVerify(&res_table, &style_analysis);
}

TEST_F(PostVerify, BundleResourceValueMergingPassTest) {
  auto tmp_dir =
      redex::make_tmp_dir("BundleResourceValueMergingPassVerify%%%%%%%%");
  boost::filesystem::path tmp_path(tmp_dir.path);
  auto base_res_dir = tmp_path / "base";
  auto local_resources_pb = base_res_dir / "resources.pb";

  const auto& source_resource_pb = resources["base/resources.pb"];
  create_directories(base_res_dir);
  redex::copy_file(source_resource_pb, local_resources_pb.string());

  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(local_resources_pb.string());

  resource_value_merging_PostVerify(&res_table);
}
