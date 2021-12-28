/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InlinerConfig.h"

#include <boost/algorithm/string/predicate.hpp>

#include "AnnoUtils.h"
#include "DexClass.h"
#include "Walkers.h"

namespace inliner {
void InlinerConfig::populate(const Scope& scope) {
  if (m_populated) {
    return;
  }
  walk::classes(scope, [this](DexClass* cls) {
    for (const auto& type_s : m_blocklist) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        blocklist.emplace(cls->get_type());
        break;
      }
    }
    for (const auto& type_s : m_caller_blocklist) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        caller_blocklist.emplace(cls->get_type());
        break;
      }
    }
    for (const auto& type_s : m_intradex_allowlist) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        intradex_allowlist.emplace(cls->get_type());
        break;
      }
    }
    // Class may be annotated by no_inline_annos.
    if (has_any_annotation(cls, m_no_inline_annos)) {
      for (auto method : cls->get_dmethods()) {
        method->rstate.set_dont_inline();
      }
      for (auto method : cls->get_vmethods()) {
        method->rstate.set_dont_inline();
      }
    }
  });
  walk::parallel::methods(scope, [this](DexMethod* method) {
    if (method->rstate.dont_inline()) {
      return;
    }
    if (has_any_annotation(method, m_no_inline_annos)) {
      method->rstate.set_dont_inline();
    } else if (has_any_annotation(method, m_force_inline_annos)) {
      method->rstate.set_force_inline();
    }
  });
  m_populated = true;
}
} // namespace inliner
