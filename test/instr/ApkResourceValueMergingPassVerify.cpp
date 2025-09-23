/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Styles.h"
#include <gtest/gtest.h>

#include "ApkResources.h"
#include "ResourceValueMergingPassVerifyImpl.h"
#include "verify/VerifyUtil.h"

TEST_F(PreVerify, ApkResourceValueMergingPassTest) {

  auto tmp_dir =
      redex::make_tmp_dir("ApkResourceValueMergingPassVerify%%%%%%%%");
  boost::filesystem::path tmp_path(tmp_dir.path);
  auto local_resources_apk = tmp_path / "resources.arsc";
  auto local_andriod_xml = tmp_path / "AndroidManifest.xml";

  const auto& source_resource_arsc = resources.at("resources.arsc");
  const auto& source_andriod_xml = resources.at("AndroidManifest.xml");

  redex::copy_file(source_resource_arsc, local_resources_apk.string());
  redex::copy_file(source_andriod_xml, local_andriod_xml.string());

  ResourcesArscFile res_table{local_resources_apk.string()};

  StyleAnalysis style_analysis = create_style_analysis(tmp_path, classes);
  resource_value_merging_PreVerify(&res_table, &style_analysis);
}

TEST_F(PostVerify, ApkResourceValueMergingPassTest) {
  auto tmp_dir =
      redex::make_tmp_dir("ApkResourceValueMergingPassVerify%%%%%%%%");
  boost::filesystem::path tmp_path(tmp_dir.path);
  auto local_resources_pb = tmp_path / "resources.arsc";

  const auto& source_resource_arsc = resources.at("resources.arsc");
  redex::copy_file(source_resource_arsc, local_resources_pb.string());

  ResourcesArscFile res_table{local_resources_pb.string()};

  resource_value_merging_PostVerify(&res_table);
}
