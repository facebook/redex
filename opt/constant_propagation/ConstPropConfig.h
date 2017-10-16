// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <unordered_set>
#include "DexClass.h"

struct ConstPropConfig {
  std::unordered_set<DexType*> blacklist;
  bool replace_moves_with_consts;
  bool fold_arithmetic;
};
