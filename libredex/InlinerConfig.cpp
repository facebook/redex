/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InlinerConfig.h"

#include <boost/algorithm/string/predicate.hpp>
#include <mutex>

#include "AnnoUtils.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "Show.h"
#include "Walkers.h"

namespace inliner {
void InlinerConfig::populate(const Scope& scope) {
  if (m_populated) {
    return;
  }

  std::mutex set_mutex;
  UnorderedSet<DexClass*> classes_matching_force_inline;

  walk::classes(scope, [&](DexClass* cls) {
    for (const std::string& type_str : blocklist) {
      if (boost::starts_with(cls->get_name()->str(), type_str)) {
        m_blocklist.emplace(cls->get_type());
        break;
      }
    }
    for (const std::string& type_str : caller_blocklist) {
      if (boost::starts_with(cls->get_name()->str(), type_str)) {
        m_caller_blocklist.emplace(cls->get_type());
        break;
      }
    }
    for (const std::string& type_str : intradex_allowlist) {
      if (boost::starts_with(cls->get_name()->str(), type_str)) {
        m_intradex_allowlist.emplace(cls->get_type());
        break;
      }
    }
    for (const std::string& str : no_inline_blocklist) {
      std::string_view class_str = cls->get_name()->str();
      std::string_view class_prefix_str = std::string_view(str).substr(
          0, std::min(str.size(), class_str.size()));
      if (boost::starts_with(class_str, class_prefix_str)) {
        for (auto* method : cls->get_all_methods()) {
          if (boost::starts_with(show(method), str)) {
            method->rstate.set_dont_inline();
          }
        }
      }
    }
    // Class may be annotated by no_inline_annos.
    if (has_any_annotation(cls, no_inline_annos)) {
      for (auto* method : cls->get_dmethods()) {
        method->rstate.set_dont_inline();
      }
      for (auto* method : cls->get_vmethods()) {
        method->rstate.set_dont_inline();
      }
    }

    for (const std::string& str : force_inline_prefixes) {
      std::string_view class_str = cls->get_name()->str();
      std::string_view fi_prefix = std::string_view(str).substr(
          0, std::min(str.size(), class_str.size()));
      if (/* class (prefix) */ class_str.starts_with(fi_prefix) ||
          /* method (prefix) */ fi_prefix.starts_with(class_str)) {
        std::unique_lock<std::mutex> lock{set_mutex};
        classes_matching_force_inline.insert(cls);
        break;
      }
    }
  });
  walk::parallel::methods(scope, [this](DexMethod* method) {
    if (method->rstate.dont_inline()) {
      return;
    }
    if (has_any_annotation(method, no_inline_annos)) {
      method->rstate.set_dont_inline();
    } else if (has_any_annotation(method, force_inline_annos)) {
      method->rstate.set_force_inline();
    }
  });

  if (!classes_matching_force_inline.empty()) {
    walk::parallel::classes(
        classes_matching_force_inline, [this](DexClass* cls) {
          for (const std::string& str : force_inline_prefixes) {
            for (auto* method : cls->get_all_methods()) {
              if (method->rstate.dont_inline()) {
                continue;
              }
              if (std::string_view(show(method)).starts_with(str)) {
                method->rstate.set_force_inline();
              }
            }
          }
        });
  }
  m_populated = true;
}

} // namespace inliner
