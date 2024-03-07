/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TestStructures.h"

#include "androidfw/ResourceTypes.h"

// Sample data for building arsc test cases
EntryAndValue e0(0, android::Res_value::TYPE_DIMENSION, 1000);
EntryAndValue e0_land(0, android::Res_value::TYPE_DIMENSION, 1001);
EntryAndValue e1(1, android::Res_value::TYPE_DIMENSION, 2000);
EntryAndValue e2(2, android::Res_value::TYPE_REFERENCE, 0x7f010001);
EntryAndValue id_0(0, android::Res_value::TYPE_INT_BOOLEAN, 0);
EntryAndValue id_1(1, android::Res_value::TYPE_INT_BOOLEAN, 0);
EntryAndValue id_2(2, android::Res_value::TYPE_INT_BOOLEAN, 0);
MapEntryAndValues style(3, 0);

// The package that many unit tests will be in.
android::ResTable_package foo_package{.id = 0x7f,
                                      .name = {'f', 'o', 'o', '\0'}};
// Create a default ResTable_config
android::ResTable_config default_config = {
    .size = sizeof(android::ResTable_config)};
// Create a landscape config
android::ResTable_config land_config = {
    .size = sizeof(android::ResTable_config),
    .orientation = android::ResTable_config::ORIENTATION_LAND};
// And a xxhdpi config
android::ResTable_config xxhdpi_config = {
    .size = sizeof(android::ResTable_config),
    .density = android::ResTable_config::DENSITY_XXHIGH};
