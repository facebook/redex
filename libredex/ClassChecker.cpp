/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <mutex>

#include "ClassChecker.h"
#include "ClassHierarchy.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "Show.h"
#include "Timer.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {

constexpr const size_t MAX_ITEMS_TO_PRINT = 20;
constexpr const char* INDENTATION = "    ";

template <typename Collection>
void print_failed_things(const Collection& items, std::ostringstream* oss) {
  size_t counter = 0;
  for (auto& fail : UnorderedIterable(items)) {
    *oss << show(fail)
         << " (deobfuscated: " << fail->get_deobfuscated_name_or_empty_copy()
         << ")\n";
    counter++;
    if (counter == MAX_ITEMS_TO_PRINT) {
      if (items.size() > MAX_ITEMS_TO_PRINT) {
        *oss << "...truncated...\n";
      }
      break;
    }
  }
}

void print_failed_external_check(
    const ConcurrentMap<const DexClass*,
                        InsertOnlyConcurrentSet<const DexType*>>&
        failed_classes_external_check,
    std::ostringstream* oss) {
  size_t counter = 0;
  for (const auto& pair : UnorderedIterable(failed_classes_external_check)) {
    *oss << "Internal class " << show(pair.first) << " (deobfuscated: "
         << pair.first->get_deobfuscated_name_or_empty_copy() << ")\n"
         << "  has external children:\n";
    for (const auto* type : UnorderedIterable(pair.second)) {
      *oss << INDENTATION << show(type) << std::endl;
    }
    counter++;
    if (counter == MAX_ITEMS_TO_PRINT) {
      if (failed_classes_external_check.size() > MAX_ITEMS_TO_PRINT) {
        *oss << "...truncated...\n";
      }
      break;
    }
  }
}

void print_failed_definition_check(
    const ConcurrentMap<const DexClass*,
                        InsertOnlyConcurrentSet<const DexType*>>&
        failed_classes_definition_check,
    std::ostringstream* oss) {
  size_t counter = 0;
  for (const auto& pair : UnorderedIterable(failed_classes_definition_check)) {
    *oss << "Internal class " << show(pair.first) << " (deobfuscated: "
         << pair.first->get_deobfuscated_name_or_empty_copy() << ")\n"
         << "  references type not defined internally or externally:\n";
    for (const auto* type : UnorderedIterable(pair.second)) {
      *oss << INDENTATION << show(type) << std::endl;
    }

    counter++;
    if (counter == MAX_ITEMS_TO_PRINT) {
      if (failed_classes_definition_check.size() > MAX_ITEMS_TO_PRINT) {
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
  for (const auto& v : child_cls->get_vmethods()) {
    NameAndProto np(v);
    auto search = final_methods.find(np);
    if (search != final_methods.end()) {
      // Check package visibility rules per
      // https://docs.oracle.com/javase/specs/jvms/se8/html/jvms-5.html#jvms-5.4.5
      const auto* super_method = search->second;
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

void ClassChecker::init_setting(
    bool definition_check,
    const UnorderedSet<std::string>& definition_check_allowlist,
    const UnorderedSet<std::string>& definition_check_allowlist_prefixes,
    bool external_check,
    const UnorderedSet<std::string>& external_check_allowlist,
    const UnorderedSet<std::string>& external_check_allowlist_prefixes) {
  m_external_check = external_check;
  m_external_check_allowlist_prefixes = external_check_allowlist_prefixes;
  m_definition_check = definition_check;
  m_definition_check_allowlist_prefixes = definition_check_allowlist_prefixes;
  for (const auto& cls_string : UnorderedIterable(definition_check_allowlist)) {
    auto* type = DexType::get_type(cls_string);
    if (type != nullptr) {
      m_definition_check_allowlist.emplace(type);
    }
  }
  for (const auto& cls_string : UnorderedIterable(external_check_allowlist)) {
    auto* type = DexType::get_type(cls_string);
    if (type != nullptr) {
      m_external_check_allowlist.emplace(type);
    }
  }
}

void ClassChecker::run(const Scope& scope) {
  std::mutex finals_mutex;
  std::unordered_map<const DexClass*, NamedMethodMap> class_to_final_methods;
  auto hierarchy = build_type_hierarchy(scope);
  UnorderedSet<const DexType*> internal_types;
  walk::classes(scope,
                [&](DexClass* cls) { internal_types.insert(cls->get_type()); });

  auto match_allowlist_prefix =
      [&](const UnorderedSet<std::string>& allowlist_prefixes,
          const DexType* type) -> bool {
    return unordered_find_if(
               allowlist_prefixes, [type](const std::string& name) {
                 return boost::starts_with(type->get_name()->str(), name);
               }) != allowlist_prefixes.end();
  };

  auto check_class_defined = [&](DexType* type) -> bool {
    if (internal_types.count(type) == 0) {
      auto* cls = type_class(type);
      if ((cls == nullptr || !cls->is_external()) &&
          m_definition_check_allowlist.find(type) ==
              m_definition_check_allowlist.end() &&
          !match_allowlist_prefix(m_definition_check_allowlist_prefixes,
                                  type)) {
        return false;
      }
    }
    return true;
  };

  {
    Timer t("ClassChecker_walk");
    walk::parallel::classes(scope, [&](DexClass* cls) {
      if (!is_interface(cls) && !is_abstract(cls)) {
        for (auto* m : cls->get_all_methods()) {
          if (is_abstract(m)) {
            m_good = false;
            m_failed_classes_abstract_check.insert(cls);
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
      if (m_external_check) {
        always_assert(!cls->is_external());
        auto child_types = get_all_children(hierarchy, cls->get_type());
        for (const auto& child_type : child_types) {
          if (internal_types.count(child_type) == 0 &&
              m_external_check_allowlist.find(child_type) ==
                  m_external_check_allowlist.end() &&
              !match_allowlist_prefix(m_external_check_allowlist_prefixes,
                                      child_type)) {
            m_good = false;
            m_failed_classes_external_check.update(
                cls, [&](auto*, auto& set, bool) { set.insert(child_type); });
          }
        }
      }

      if (m_definition_check) {
        auto* super_type = cls->get_super_class();
        if (!check_class_defined(super_type)) {
          m_good = false;
          m_failed_classes_definition_check.update(
              cls, [&](auto*, auto& set, bool) { set.insert(super_type); });
        }
        for (const auto& intf : *cls->get_interfaces()) {
          if (!check_class_defined(intf)) {
            m_good = false;
            m_failed_classes_definition_check.update(
                cls, [&](auto*, auto& set, bool) { set.insert(intf); });
          }
        }
      }
    });
  }
  {
    Timer t("ClassChecker_hierarchy_traverse");
    for (auto&& [cls, final_methods] : class_to_final_methods) {
      auto child_types = get_all_children(hierarchy, cls->get_type());
      for (const auto& child_type : child_types) {
        auto* child_cls = type_class(child_type);
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
  std::ostringstream oss;
  if (!m_failed_classes_external_check.empty()) {
    oss << "External classes with internal class hierarchy (likely dependency "
           "setting issue if fail at input):"
        << std::endl;
    print_failed_external_check(m_failed_classes_external_check, &oss);
    oss << std::endl;
  }
  if (!m_failed_classes_definition_check.empty()) {
    oss << "Class reference type not defined (likely dependency setting issue "
           "if fail at input):"
        << std::endl;
    print_failed_definition_check(m_failed_classes_definition_check, &oss);
    oss << std::endl;
  }
  if (!m_failed_classes_abstract_check.empty()) {
    oss << "Nonabstract classes with abstract methods:" << std::endl;
    print_failed_things(m_failed_classes_abstract_check, &oss);
    oss << std::endl;
  }
  if (!m_failed_methods.empty()) {
    oss << "Methods incorrectly overriding super class final method:"
        << std::endl;
    print_failed_things(m_failed_methods, &oss);
    oss << std::endl;
  }
  return oss;
}
