/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <ostream>
#include <set>
#include <vector>

namespace optimize_enums {

enum class UnsafeType {
  kNotFinal,
  kCannotDelete,
  kHasInterfaces,
  kMoreThanOneSynthField,
  kMultipleCtors,
  kComplexCtor,
  kUnrenamableDmethod,
  kUnrenamableVmethod,
  kComplexField,
  kUsage,
};

inline std::ostream& operator<<(std::ostream& os, const UnsafeType& u) {
  switch (u) {
  case UnsafeType::kNotFinal:
    os << "NotFinal";
    break;
  case UnsafeType::kCannotDelete:
    os << "CannotDelete";
    break;
  case UnsafeType::kHasInterfaces:
    os << "HasInterfaces";
    break;
  case UnsafeType::kMoreThanOneSynthField:
    os << "MoreThanOneSynthField";
    break;
  case UnsafeType::kMultipleCtors:
    os << "MultipleCtors";
    break;
  case UnsafeType::kComplexCtor:
    os << "ComplexCtor";
    break;
  case UnsafeType::kUnrenamableDmethod:
    os << "UnrenamableDmethod";
    break;
  case UnsafeType::kUnrenamableVmethod:
    os << "UnrenamableVmethod";
    break;
  case UnsafeType::kComplexField:
    os << "ComplexField";
    break;
  case UnsafeType::kUsage:
    os << "Usage";
    break;
  }
  return os;
}

using UnsafeTypes = std::set<UnsafeType>;

inline std::ostream& operator<<(std::ostream& os, const UnsafeTypes& u) {
  std::vector<UnsafeType> vec(u.begin(), u.end());
  std::sort(vec.begin(), vec.end());
  for (auto t : vec) {
    os << " ";
    os << t;
  }
  return os;
}

} // namespace optimize_enums
