/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NoOptimizationsMatcher.h"
#include "Walkers.h"

namespace keep_rules {

void process_no_optimizations_rules(
    const std::unordered_set<DexType*>& no_optimizations_annos,
    const std::unordered_set<std::string>& no_optimizations_blocklist,
    const Scope& scope) {
  auto match = m::any_annos<DexMethod>(
      m::as_type<DexAnnotation>(m::in<DexType*>(no_optimizations_annos)));
  auto is_blocklisted = [&](DexClass* cls) {
    while (cls != nullptr) {
      for (const auto& type_s : no_optimizations_blocklist) {
        if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
          return true;
        }
      }
      cls = type_class(cls->get_super_class());
    }
    return false;
  };
  walk::parallel::classes(scope, [&](DexClass* cls) {
    auto blocklisted = is_blocklisted(cls);
    auto process_method = [&](DexMethod* method) {
      if (blocklisted || match.matches(method)) {
        method->rstate.set_no_optimizations();
      }
    };
    for (auto m : cls->get_vmethods()) {
      process_method(m);
    }
    for (auto m : cls->get_dmethods()) {
      process_method(m);
    }
  });
}

} // namespace keep_rules
