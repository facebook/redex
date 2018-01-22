// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <unordered_set>
#include "DexClass.h"

struct ConstPropConfig {
  std::unordered_set<DexType*> blacklist;
  bool replace_moves_with_consts{false};
  bool fold_arithmetic{false};
  bool propagate_conditions{false};
  bool include_virtuals{false};
  bool dynamic_input_checks{false};
};
