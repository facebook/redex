/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObfuscateUtils.h"
#include "DexClass.h"
#include "Trace.h"
#include "Walkers.h"
#include <algorithm>

namespace obfuscate_utils {
static inline char get_ident_52(int num) {
  if (num < 26) {
    return 'A' + num;
  }
  num -= 26;

  always_assert(num < 26);
  return 'a' + num;
}

static inline char get_ident_62(int num) {
  if (num < 10) {
    return '0' + num;
  }
  num -= 10;

  return get_ident_52(num);
}

void compute_identifier(int value, std::string* res) {
  always_assert(res);
  always_assert(res->empty());

  // We don't want leading digits, as that causes sorting issues with
  // <clinit> and <init>.
  // Also, ensure that identifiers are at least 3 digits long, so that the
  // most frequent identifiers are lexicographically sorted, occupying likely
  // consecutive string ids, which is best for compression.
  while (value >= 52 || res->size() < 2) {
    res->append(1, get_ident_62(value % 62));
    value /= 62;
  }
  res->append(1, get_ident_52(value % 52));
  std::reverse(res->begin(), res->end());
  if (res->size() > 3) {
    // rare in practice; we put those after all other 3-character identifiers
    // so that they don't interfere with the order of 3-character identifiers
    res->insert(0, "zzz");
    TRACE(OBFUSCATE, 1, "Long identifier: %s", res->c_str());
  }
  always_assert(res->size() >= 3);
}
} // namespace obfuscate_utils

DexFieldManager new_dex_field_manager() {
  return DexFieldManager(
      [](DexField*& f) -> FieldNameWrapper* { return new FieldNameWrapper(f); },
      [](DexFieldRef* f) -> DexType* { return f->get_type(); },
      [](std::string_view new_name) -> DexFieldSpec {
        DexFieldSpec spec;
        spec.name = DexString::make_string(new_name);
        return spec;
      });
}

DexMethodManager new_dex_method_manager() {
  return DexMethodManager(
      [](DexMethod*& f) -> MethodNameWrapper* {
        return new MethodNameWrapper(f);
      },
      [](DexMethodRef* m) -> DexProto* { return m->get_proto(); },
      [](std::string_view new_name) -> DexMethodSpec {
        DexMethodSpec spec;
        spec.name = DexString::make_string(new_name);
        return spec;
      });
}
