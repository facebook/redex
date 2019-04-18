/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InlinerConfig.h"

#include <boost/algorithm/string/predicate.hpp>

#include "DexClass.h"
#include "Walkers.h"

namespace inliner {
void InlinerConfig::populate_blacklist(const Scope& scope) {
  if (populated) {
    return;
  }
  walk::classes(scope, [this](const DexClass* cls) {
    for (const auto& type_s : m_black_list) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        black_list.emplace(cls->get_type());
        break;
      }
    }
    for (const auto& type_s : m_caller_black_list) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        caller_black_list.emplace(cls->get_type());
        break;
      }
    }
  });
  populated = true;
}
} // namespace inliner
