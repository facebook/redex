/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "DexClass.h"

class FbjniMarker {
 public:
  DexType* process_class_path(const std::string& class_path);
  DexField* process_field(DexType* type, const std::string& field_str);
  DexMethod* process_method(DexType* type, const std::string& method_str);

 private:
  std::string to_internal_type(const std::string& str);

  std::unordered_set<DexType*> types;
};

void mark_native_classes_from_fbjni_configs(
    const std::vector<std::string>& json_files);
