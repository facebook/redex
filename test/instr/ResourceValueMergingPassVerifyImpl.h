/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gtest/gtest.h>

#include "RedexResources.h"
#include "Styles.h"
#include "verify/VerifyUtil.h"

StyleAnalysis create_style_analysis(const boost::filesystem::path& tmp_path,
                                    const Scope& classes);

void resource_value_merging_PreVerify(ResourceTableFile* res_table,
                                      StyleAnalysis* style_analysis);

void resource_value_merging_PostVerify(ResourceTableFile* res_table);
