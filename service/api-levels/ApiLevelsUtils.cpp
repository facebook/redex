/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApiLevelsUtils.h"

#include <boost/algorithm/string.hpp>
#include <fstream>

#include "DexClass.h"
#include "MethodOverrideGraph.h"
#include "Show.h"
#include "Trace.h"
#include "TypeReference.h"
#include "TypeSystem.h"

namespace api {

namespace {

/**
 * Lcom/facebook/something/ClassName$Foo; -> ClassName$Foo
 *
 * TODO(emmasevastian): Move it to utils.
 */
std::string get_simple_deobfuscated_name(const DexType* type) {
  auto* cls = type_class(type);
  std::string full_name;
  if (cls) {
    full_name = cls->get_deobfuscated_name();
  }
  if (full_name.empty()) {
    full_name = type->str();
  }

  size_t simple_name_pos = full_name.rfind('/');
  always_assert(simple_name_pos != std::string::npos);
  return full_name.substr(simple_name_pos + 1,
                          full_name.size() - simple_name_pos - 2);
}

/**
 * This utils handles both:
 * - filtering of types with the same simple name
 * - creation of mapping from simple_name to type
 */
std::unordered_map<std::string, const DexType*>
get_simple_cls_name_to_accepted_types(
    const std::unordered_map<const DexType*, FrameworkAPI>&
        framework_cls_to_api) {

  std::vector<std::string> filter;
  std::unordered_map<std::string, const DexType*> simple_cls_name_to_type;
  for (const auto& pair : framework_cls_to_api) {
    auto simple_name = get_simple_deobfuscated_name(pair.first);

    // For now, excluding types that have the same simple name.
    // TODO(emmasevastian): Hacky! Do this better!
    const auto& inserted =
        simple_cls_name_to_type.emplace(simple_name, pair.first);
    bool insertion_happened = inserted.second;
    if (!insertion_happened) {
      TRACE(API_UTILS, 5,
            "Excluding %s since we have similar class once already!",
            SHOW(pair.first));
      filter.emplace_back(simple_name);
    }
  }

  for (const std::string& str : filter) {
    simple_cls_name_to_type.erase(str);
  }

  return simple_cls_name_to_type;
}

} // namespace

namespace {

/**
 * When checking if a method of a release class exists in the framework
 * equivalent, checking directly the replaced version (as in replacing all
 * arguments / return value that will be replaced in the end).
 */
bool check_methods(
    const std::vector<DexMethod*>& methods,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework,
    const std::unordered_set<DexMethodRef*>& methods_non_private) {
  if (methods.empty()) {
    return true;
  }

  DexType* current_type = methods.at(0)->get_class();
  for (DexMethod* meth : methods) {
    if (methods_non_private.count(meth) == 0) {
      continue;
    }

    auto* new_proto =
        type_reference::get_new_proto(meth->get_proto(), release_to_framework);
    // NOTE: For now, this assumes no obfuscation happened. We need to update
    //       it, if it runs later.
    if (!framework_api.has_method(meth->get_simple_deobfuscated_name(),
                                  new_proto, meth->get_access())) {
      TRACE(API_UTILS, 4,
            "Excluding %s since we couldn't find corresponding method: %s!",
            SHOW(framework_api.cls), show_deobfuscated(meth).c_str());
      return false;
    }
  }

  return true;
}

bool find_field(const std::string& simple_deobfuscated_name,
                const std::vector<FRefInfo>& frefs_info,
                DexType* field_type,
                DexAccessFlags access_flags) {
  for (const FRefInfo& fref_info : frefs_info) {
    auto* fref = fref_info.fref;
    if (fref->get_name()->str() == simple_deobfuscated_name &&
        fref->get_type() == field_type) {

      // We also need to check the access flags.
      // NOTE: We accept cases where the methods are not declared final.
      if (access_flags == fref_info.access_flags ||
          (access_flags & ~ACC_FINAL) == fref_info.access_flags) {
        return true;
      }
    }
  }

  return false;
}

bool check_fields(
    const std::vector<DexField*>& fields,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework,
    const std::unordered_set<DexFieldRef*>& fields_non_private) {
  if (fields.empty()) {
    return true;
  }

  DexType* current_type = fields.at(0)->get_class();
  for (DexField* field : fields) {
    if (fields_non_private.count(field) == 0) {
      continue;
    }

    auto* field_type = field->get_type();
    auto it = release_to_framework.find(field_type);

    auto* new_field_type = field_type;
    if (it != release_to_framework.end()) {
      new_field_type = it->second;
    }

    if (!find_field(field->get_simple_deobfuscated_name(),
                    framework_api.frefs_info, new_field_type,
                    field->get_access())) {
      TRACE(API_UTILS, 4,
            "Excluding %s since we couldn't find corresponding field: %s!",
            SHOW(framework_api.cls), show_deobfuscated(field).c_str());
      return false;
    }
  }

  return true;
}

/**
 * Checks that all public members (for now) of release class, exist in
 * compatibility class.
 */
bool check_members(
    DexClass* cls,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework,
    const std::unordered_set<DexMethodRef*>& methods_non_private,
    const std::unordered_set<DexFieldRef*>& fields_non_private) {
  if (!check_methods(cls->get_dmethods(), framework_api, release_to_framework,
                     methods_non_private)) {
    return false;
  }
  if (!check_methods(cls->get_vmethods(), framework_api, release_to_framework,
                     methods_non_private)) {
    return false;
  }

  if (!check_fields(cls->get_sfields(), framework_api, release_to_framework,
                    fields_non_private)) {
    return false;
  }
  if (!check_fields(cls->get_ifields(), framework_api, release_to_framework,
                    fields_non_private)) {
    return false;
  }

  return true;
}

bool check_if_present(
    const TypeSet& types,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework) {
  for (const DexType* type : types) {
    DexClass* cls = type_class(type);
    if (!cls || cls->is_external()) {
      // TODO(emmasevastian): When it isn't safe to continue here?
      continue;
    }

    if (!release_to_framework.count(type)) {
      return false;
    }
  }

  return true;
}

bool check_hierarchy(
    DexClass* cls,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework,
    const TypeSystem& type_system,
    const std::unordered_set<const DexType*>& framework_classes) {
  DexType* type = cls->get_type();
  if (!is_interface(cls)) {
    // We don't need to worry about subclasses, as those we just need to update
    // the superclass for.
    // TODO(emmasevastian): Any case when we should worry about subclasses?

    const auto& implemented_intfs =
        type_system.get_implemented_interfaces(type);
    if (!check_if_present(implemented_intfs, release_to_framework)) {
      TRACE(API_UTILS, 4,
            "Excluding %s since we couldn't find one of the corresponding "
            "interfaces!",
            SHOW(framework_api.cls));
      return false;
    }

    auto* super_cls = cls->get_super_class();
    auto* framwork_super_cls = framework_api.super_cls;

    // We accept ONLY classes that have the super class as the corresponding
    // framework ones. It might extend existing framework class or
    // release class.
    if (framework_classes.count(super_cls) > 0) {
      if (super_cls != framwork_super_cls) {
        TRACE(API_UTILS, 4,
              "Excluding %s since the class had different superclass than %s!",
              SHOW(framework_api.cls), show_deobfuscated(super_cls).c_str());
        return false;
      }
    } else if (release_to_framework.count(super_cls) == 0 ||
               framwork_super_cls != release_to_framework.at(super_cls)) {
      TRACE(API_UTILS, 4,
            "Excluding %s since we couldn't find the corresponding superclass "
            "%s!",
            SHOW(framework_api.cls), show_deobfuscated(super_cls).c_str());
      return false;
    }
  } else {
    TypeSet super_intfs;
    type_system.get_all_super_interfaces(type, super_intfs);

    if (!check_if_present(super_intfs, release_to_framework)) {
      TRACE(API_UTILS, 4,
            "Excluding %s since we couldn't find one of the corresponding "
            "extended interfaces!",
            SHOW(framework_api.cls));
      return false;
    }
  }

  return true;
}

} // namespace

/**
 * Check that the replacements are valid:
 * - release library to framework classes have the same public members
 * - we have entire hierarchies (as in up the hierarchy, since subclasses
 *                               we can update)
 *
 * TODO(emmasevastian): Add extra checks: non public members? etc
 */
void ApiLevelsUtils::check_and_update_release_to_framework(const Scope& scope) {
  TypeSystem type_system(scope);

  // We need to check this in a loop, as an exclusion might have dependencies.
  while (true) {
    std::unordered_set<const DexType*> to_remove;

    // We need an up to date pairing from release library to framework classes,
    // for later use. So computing this on the fly, once.
    std::unordered_map<const DexType*, DexType*> release_to_framework;
    for (const auto& pair : m_types_to_framework_api) {
      release_to_framework[pair.first] = pair.second.cls;
    }

    for (const auto& pair : m_types_to_framework_api) {
      DexClass* cls = type_class(pair.first);
      always_assert(cls);

      if (cls->get_access() != pair.second.access_flags) {
        TRACE(API_UTILS, 5,
              "Excluding %s since it has different access flags "
              "than the framework class: %d vs %d",
              show_deobfuscated(cls).c_str(), cls->get_access(),
              pair.second.access_flags);
        to_remove.emplace(pair.first);
        continue;
      }

      if (!check_members(cls, pair.second, release_to_framework,
                         m_methods_non_private, m_fields_non_private)) {
        to_remove.emplace(pair.first);
        continue;
      }

      if (!check_hierarchy(cls, pair.second, release_to_framework, type_system,
                           m_framework_classes)) {
        to_remove.emplace(pair.first);
      }
    }

    if (to_remove.empty()) {
      break;
    }

    for (const DexType* type : to_remove) {
      m_types_to_framework_api.erase(type);
    }
  }
}

void ApiLevelsUtils::gather_non_private_members(const Scope& scope) {
  m_methods_non_private.clear();
  m_fields_non_private.clear();

  const auto& override_graph = method_override_graph::build_graph(scope);

  // TODO(emmasevastian): parallelize.
  for (DexClass* cls : scope) {
    std::vector<DexMethodRef*> current_methods;
    std::vector<DexFieldRef*> current_fields;

    cls->gather_methods(current_methods);
    for (DexMethodRef* mref : current_methods) {
      if (m_types_to_framework_api.count(mref->get_class())) {
        if (mref->get_class() != cls->get_type()) {
          m_methods_non_private.emplace(mref);
        } else {
          auto* mdef = mref->as_def();

          // Being extra conservative here ...
          // NOTE: Whatever we add to the list we will need to replace.
          if (!mdef ||
              method_override_graph::is_true_virtual(*override_graph, mdef)) {
            m_methods_non_private.emplace(mref);
          }
        }
      }
    }

    cls->gather_fields(current_fields);
    for (DexFieldRef* fref : current_fields) {
      if (m_types_to_framework_api.count(fref->get_class()) &&
          fref->get_class() != cls->get_type()) {
        m_fields_non_private.emplace(fref);
      }
    }
  }

  TRACE(API_UTILS, 4, "We have %d methods that are actually non private",
        m_methods_non_private.size());
  TRACE(API_UTILS, 4, "We have %d fields that are actually non private",
        m_fields_non_private.size());
}

void ApiLevelsUtils::load_framework_api(const Scope& scope) {
  auto framework_cls_to_api = get_framework_classes();
  for (auto it = framework_cls_to_api.begin();
       it != framework_cls_to_api.end();) {
    auto* framework_cls = it->first;
    m_framework_classes.emplace(framework_cls);

    // NOTE: We are currently excluding classes outside of
    //       android package. We might reconsider.
    const auto& framework_cls_str = framework_cls->str();
    if (!boost::starts_with(framework_cls_str, "Landroid")) {
      TRACE(API_UTILS, 5, "Excluding %s from possible replacement.",
            framework_cls_str.c_str());
      it = framework_cls_to_api.erase(it);
    } else {
      ++it;
    }
  }

  std::unordered_map<std::string, const DexType*> simple_cls_name_to_type =
      get_simple_cls_name_to_accepted_types(framework_cls_to_api);
  if (simple_cls_name_to_type.empty()) {
    // Nothing to do here :|
    TRACE(
        API_UTILS, 1,
        "Nothing to do since we have no framework classes to replace with ...");
    return;
  }

  std::unordered_set<std::string> simple_names_releases;
  for (DexClass* cls : scope) {
    if (cls->is_external()) {
      continue;
    }

    const auto& cls_str = cls->get_deobfuscated_name();

    // TODO(emmasevastian): Better way of detecting release libraries ...
    if (boost::starts_with(cls_str, "Landroidx")) {
      std::string simple_name = get_simple_deobfuscated_name(cls->get_type());
      auto simple_cls_it = simple_cls_name_to_type.find(simple_name);
      if (simple_cls_it == simple_cls_name_to_type.end()) {
        TRACE(API_UTILS, 7,
              "Release library class %s has no corresponding framework class.",
              show_deobfuscated(cls).c_str());
        continue;
      }

      // Assume there are no classes with the same simple name.
      // TODO(emmasevastian): Reconsider this! For now, leaving it as using
      //                      simple name, since paths have changed between
      //                      release and compatibility libraries.
      auto inserted = simple_names_releases.emplace(simple_name);
      if (!inserted.second) {
        m_types_to_framework_api.erase(cls->get_type());
      } else {
        m_types_to_framework_api[cls->get_type()] =
            std::move(framework_cls_to_api[simple_cls_it->second]);
      }
    }
  }

  gather_non_private_members(scope);

  // Checks and updates the mapping from release libraries to framework classes.
  check_and_update_release_to_framework(scope);
}

void ApiLevelsUtils::filter_types(
    const std::unordered_set<const DexType*>& types, const Scope& scope) {
  for (const auto* type : types) {
    m_types_to_framework_api.erase(type);
  }

  // Make sure we clean up the dependencies.
  check_and_update_release_to_framework(scope);
}

} // namespace api
