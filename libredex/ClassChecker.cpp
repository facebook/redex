/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <mutex>

#include "ClassChecker.h"
#include "ClassHierarchy.h"
#include "DexClass.h"
#include "Show.h"
#include "Timer.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {
template <typename Collection>
void print_failed_things(const Collection& items,
                         const size_t print_limit,
                         std::ostringstream* oss) {
  size_t counter = 0;
  for (auto& fail : items) {
    *oss << show(fail)
         << " (deobfuscated: " << fail->get_deobfuscated_name_or_empty_copy()
         << ")\n";
    counter++;
    if (counter == print_limit) {
      if (items.size() > print_limit) {
        *oss << "...truncated...\n";
      }
      break;
    }
  }
}

struct NameAndProto {
  const DexString* name;
  const DexProto* proto;
  explicit NameAndProto(const DexMethod* m)
      : name(m->get_name()), proto(m->get_proto()) {}
};

struct compare_name_and_proto {
  bool operator()(const NameAndProto& l, const NameAndProto& r) const {
    if (l.name == r.name) {
      return compare_dexprotos(l.proto, r.proto);
    }
    return compare_dexstrings(l.name, r.name);
  }
};

using NamedMethodMap =
    std::map<NameAndProto, const DexMethod*, compare_name_and_proto>;

template <typename Collection>
bool has_colliding_methods(const DexClass* cls,
                           const NamedMethodMap& final_methods,
                           const DexClass* child_cls,
                           Collection* failures) {
  bool result{false};
  for (auto& v : child_cls->get_vmethods()) {
    NameAndProto np(v);
    auto search = final_methods.find(np);
    if (search != final_methods.end()) {
      // Check package visibility rules per
      // https://docs.oracle.com/javase/specs/jvms/se8/html/jvms-5.html#jvms-5.4.5
      auto super_method = search->second;
      if (is_public(super_method) || is_protected(super_method) ||
          type::same_package(cls->get_type(), child_cls->get_type())) {
        result = true;
        failures->insert(v);
      }
    }
  }
  return result;
}
} // namespace

ClassChecker::ClassChecker() : m_good(true) {}

void ClassChecker::run(const Scope& scope) {
  std::mutex finals_mutex;
  std::unordered_map<const DexClass*, NamedMethodMap> class_to_final_methods;
  {
    Timer t("ClassChecker_walk");
    walk::parallel::classes(scope, [&](DexClass* cls) {
      if (!is_interface(cls) && !is_abstract(cls)) {
        for (auto* m : cls->get_all_methods()) {
          if (is_abstract(m)) {
            m_good = false;
            m_failed_classes.insert(cls);
            return;
          }
        }
      }
      if (!is_interface(cls)) {
        for (auto* m : cls->get_vmethods()) {
          if (is_final(m)) {
            std::lock_guard<std::mutex> lock(finals_mutex);
            NameAndProto np(m);
            class_to_final_methods[cls].emplace(np, m);
          }
        }
      }
    });
  }
  {
    Timer t("ClassChecker_hierarchy_traverse");
    auto hierarchy = build_internal_type_hierarchy(scope);
    for (auto&& [cls, final_methods] : class_to_final_methods) {
      auto child_types = get_all_children(hierarchy, cls->get_type());
      for (const auto& child_type : child_types) {
        auto child_cls = type_class(child_type);
        always_assert(child_cls != nullptr);
        if (has_colliding_methods(
                cls, final_methods, child_cls, &m_failed_methods)) {
          m_good = false;
        }
      }
    }
  }
}

std::ostringstream ClassChecker::print_failed_classes() {
  constexpr size_t MAX_ITEMS_TO_PRINT = 10;
  std::ostringstream oss;
  if (!m_failed_classes.empty()) {
    oss << "Nonabstract classes with abstract methods:" << std::endl;
    print_failed_things(m_failed_classes, MAX_ITEMS_TO_PRINT, &oss);
  }
  if (!m_failed_methods.empty()) {
    oss << "Methods incorrectly overriding super class final method:"
        << std::endl;
    print_failed_things(m_failed_methods, MAX_ITEMS_TO_PRINT, &oss);
  }
  return oss;
}
