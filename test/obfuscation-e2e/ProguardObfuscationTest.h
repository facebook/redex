/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Match.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "ReachableClasses.h"
#include "RedexContext.h"

class ProguardObfuscationTest {
 private:
  ProguardMap proguard_map;
  // Classes we're looking at will always be at dexen.front()
  std::vector<DexClasses> dexen;
  int method_is_renamed_helper(const std::vector<DexMethod*>& methods,
                               const std::string& name);

 public:
  ProguardObfuscationTest(const char* dexfile, const char* mapping_file);

  bool configure_proguard(const char* configuration_file);

  DexClass* find_class_named(const std::string& name);

  bool field_found(const std::vector<DexField*>& fields,
                   const std::string& name);

  bool method_is_renamed(const DexClass* cls, const std::string& name);

  bool refs_to_field_found(const std::string& name);
};
