/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/regex.hpp>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "DexAccess.h"
#include "DexUtil.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardRegex.h"
#include "ReachableClasses.h"

namespace redex {

// Updatea a class, field or method to add keep modifiers.
template <class DexMember>
void apply_keep_modifiers(KeepSpec& k, DexMember* member) {
  if (k.includedescriptorclasses) {
    member->rstate.set_includedescriptorclasses();
  }
  if (k.allowoptimization) {
    member->rstate.set_allowoptimization();
  }
  // Only set allowshrinking when no other keep has been applied to this
  // class or member.
  if (k.allowshrinking) {
    if (!keep(member)) {
      member->rstate.set_allowshrinking();
    }
  } else {
    // Otherwise reset it.
    member->rstate.unset_allowshrinking();
  }
  // Only set allowobfuscation when no other keep has bee applied to this
  // class or member.
  if (k.allowobfuscation) {
    if (!keep(member) && strcmp(member->get_name()->c_str(), "<init>") != 0) {
      member->rstate.set_allowobfuscation();
    }
  } else {
    // Otherwise reset it.
    member->rstate.unset_allowobfuscation();
  }
}

// Is this keep_rule an application of a blanket top-level keep
// -keep,allowshrinking class * rule?
inline bool is_blanket_keep_rule(const KeepSpec& keep_rule) {
  if (keep_rule.allowshrinking) {
    const auto& spec = keep_rule.class_spec;
    if (spec.className == "*" && spec.annotationType == "" &&
        spec.fieldSpecifications.empty() && spec.methodSpecifications.empty() &&
        spec.extendsAnnotationType == "" && spec.extendsClassName == "" &&
        spec.setAccessFlags == 0 && spec.unsetAccessFlags == 0) {
      return true;
    }
  }
  return false;
}

bool check_required_access_flags(const DexAccessFlags requiredSet,
                                 const DexAccessFlags access_flags) {
  const DexAccessFlags access_mask = ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED;
  const DexAccessFlags required_set_flags = requiredSet & ~access_mask;
  const DexAccessFlags required_one_set_flags = requiredSet & access_mask;
  return (required_set_flags & ~access_flags) == 0 &&
           (required_one_set_flags == 0 ||
           (required_one_set_flags & access_flags) != 0);
}

bool check_required_unset_access_flags(
    const DexAccessFlags requiredUnset,
    const DexAccessFlags access_flags) {
  return (requiredUnset & access_flags) == 0;
}

boost::regex* register_matcher(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const std::string& regex) {
  auto where = regex_map.find(regex);
  if (where == regex_map.end()) {
    boost::regex* matcher = new boost::regex(regex);
    regex_map.emplace(regex, matcher);
    return matcher;
  }
  return where->second;
}

template <class DexMember>
bool has_annotation(std::unordered_map<std::string, boost::regex*>& regex_map,
                    const DexMember* member,
                    const std::string& annotation) {
  auto annos = member->get_anno_set();
  if (annos != nullptr) {
    auto annotation_regex = proguard_parser::form_type_regex(annotation);
    boost::regex* annotation_matcher =
        register_matcher(regex_map, annotation_regex);
    for (const auto& anno : annos->get_annotations()) {
      if (boost::regex_match(anno->type()->c_str(), *annotation_matcher)) {
        return true;
      }
    }
  }
  return false;
}

bool access_matches(const DexAccessFlags requiredSet,
                    const DexAccessFlags requiredUnset,
                    const DexAccessFlags access_flags) {
  return check_required_access_flags(requiredSet, access_flags) &&
         check_required_unset_access_flags(requiredUnset, access_flags);
}

// From a fully qualified descriptor for a field, exract just the
// name of the field which occurs between the ;. and : characters.
std::string extract_field_name(std::string qualified_fieldname) {
  auto p = qualified_fieldname.find(";.");
  if (p == std::string::npos) {
    return qualified_fieldname;
  }
  return qualified_fieldname.substr(p + 2);
}

std::string extract_method_name_and_type(std::string qualified_fieldname) {
  auto p = qualified_fieldname.find(";.");
  return qualified_fieldname.substr(p + 2);
}

bool field_level_match(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    redex::MemberSpecification& fieldSpecification,
    DexField* field,
    const boost::regex* fieldname_regex) {
  // Check for annotation guards.
  if (!(fieldSpecification.annotationType.empty())) {
    if (!has_annotation(regex_map, field, fieldSpecification.annotationType)) {
      return false;
    }
  }
  // Check for access match.
  if (!access_matches(fieldSpecification.requiredSetAccessFlags,
                      fieldSpecification.requiredUnsetAccessFlags,
                      field->get_access())) {
    return false;
  }
  // Match field name against regex.
  auto dequalified_name = extract_field_name(field->get_deobfuscated_name());
  return boost::regex_match(dequalified_name, *fieldname_regex);
}

void keep_fields(std::unordered_map<std::string, boost::regex*>& regex_map,
                 redex::KeepSpec& keep_rule,
                 const bool apply_modifiers,
                 const std::list<DexField*>& fields,
                 redex::MemberSpecification& fieldSpecification,
                 const boost::regex* fieldname_regex) {
  for (auto field : fields) {
    if (field_level_match(
            regex_map, fieldSpecification, field, fieldname_regex)) {
      if (apply_modifiers) {
        apply_keep_modifiers(keep_rule, field);
      }
      field->rstate.set_keep();
      fieldSpecification.count++;
    }
  }
}

std::string field_regex(const MemberSpecification& field_spec) {
  std::stringstream ss;
  ss << proguard_parser::form_member_regex(field_spec.name);
  ss << "\\:";
  ss << proguard_parser::form_type_regex(field_spec.descriptor);
  return ss.str();
}

void apply_field_keeps(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const DexClass* cls,
    redex::KeepSpec& keep_rule,
    const bool apply_modifiers) {
  for (auto& field_spec : keep_rule.class_spec.fieldSpecifications) {
    auto fieldname_regex = field_regex(field_spec);
    boost::regex* matcher = register_matcher(regex_map, fieldname_regex);
    keep_fields(regex_map,
                keep_rule,
                apply_modifiers,
                cls->get_ifields(),
                field_spec,
                matcher);
    keep_fields(regex_map,
                keep_rule,
                apply_modifiers,
                cls->get_sfields(),
                field_spec,
                matcher);
  }
}

bool method_level_match(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    redex::MemberSpecification& methodSpecification,
    DexMethod* method,
    const boost::regex* method_regex) {
  // Check to see if the method match is guarded by an annotaiton match.
  if (!(methodSpecification.annotationType.empty())) {
    if (!has_annotation(
            regex_map, method, methodSpecification.annotationType)) {
      return false;
    }
  }
  if (!access_matches(methodSpecification.requiredSetAccessFlags,
                      methodSpecification.requiredUnsetAccessFlags,
                      method->get_access())) {
    return false;
  }
  auto dequalified_name =
      extract_method_name_and_type(method->get_deobfuscated_name());
  return boost::regex_match(dequalified_name.c_str(), *method_regex);
}

void keep_clinits(DexClass* cls) {
  for (auto method : cls->get_dmethods()) {
    if (is_clinit(method)) {
      if (((method->get_code() != nullptr) &&
           method->get_code()->get_instructions().size() > 1)) {
        method->rstate.set_keep();
        break;
      }
    }
  }
}

void keep_methods(std::unordered_map<std::string, boost::regex*>& regex_map,
                  KeepSpec& keep_rule,
                  const bool apply_modifiers,
                  redex::MemberSpecification& methodSpecification,
                  const std::list<DexMethod*>& methods,
                  const boost::regex* method_regex,
                  std::function<void(DexMethod*)> keeper) {
  for (const auto& method : methods) {
    if (method_level_match(
            regex_map, methodSpecification, method, method_regex)) {
      if (apply_modifiers) {
        apply_keep_modifiers(keep_rule, method);
      }
      keeper(method);
      methodSpecification.count++;
    }
  }
}

std::string method_regex(const MemberSpecification& method_spec) {
  auto qualified_method_regex =
      proguard_parser::form_member_regex(method_spec.name);
  qualified_method_regex += "\\:";
  qualified_method_regex +=
      proguard_parser::form_type_regex(method_spec.descriptor);
  return qualified_method_regex;
}

void apply_method_keeps(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const DexClass* cls,
    redex::KeepSpec& keep_rule,
    const bool apply_modifiers,
    std::function<void(DexMethod*)> keeper) {
  auto methodSpecifications = keep_rule.class_spec.methodSpecifications;
  for (auto& method_spec : methodSpecifications) {
    auto qualified_method_regex = method_regex(method_spec);
    boost::regex* method_regex =
        register_matcher(regex_map, qualified_method_regex);
    keep_methods(regex_map,
                 keep_rule,
                 apply_modifiers,
                 method_spec,
                 cls->get_vmethods(),
                 method_regex,
                 keeper);
    keep_methods(regex_map,
                 keep_rule,
                 apply_modifiers,
                 method_spec,
                 cls->get_dmethods(),
                 method_regex,
                 keeper);
  }
}

// This function checks to see of a class satisfies a match
// against an annotation type and a class wildcard.
bool type_and_annotation_match(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const DexClass* cls,
    const std::string& extends_class_name,
    const std::string& annotation) {
  if (cls == nullptr) {
    return false;
  }
  if (cls->get_type() == get_object_type()) {
    return false;
  }
  // First check to see if an annotation type needs to be matched.
  if (!(annotation.empty())) {
    if (!has_annotation(regex_map, cls, annotation)) {
      return false;
    }
  }
  // Now try to match against the class name.
  auto deob_name = cls->get_deobfuscated_name();
  auto descriptor = proguard_parser::convert_wildcard_type(extends_class_name);
  auto desc_regex = proguard_parser::form_type_regex(descriptor);
  boost::regex* matcher = register_matcher(regex_map, desc_regex);
  return boost::regex_match(deob_name, *matcher);
}

bool search_extends_and_interfaces(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    std::set<const DexClass*>* visited,
    const DexClass* cls,
    const std::string& extends_class_name,
    const std::string& annotation);

bool search_interfaces(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    std::set<const DexClass*>* visited,
    const DexClass* cls,
    const std::string& name,
    const std::string& annotation) {
  auto interfaces = cls->get_interfaces();
  if (interfaces) {
    for (const auto& impl : interfaces->get_type_list()) {
      auto impl_class = type_class(impl);
      if (impl_class) {
        if (search_extends_and_interfaces(
                regex_map, visited, impl_class, name, annotation)) {
          return true;
        }
      } else {
        TRACE(PGR, 8, "WARNING: Can't find class for type %s\n", impl->c_str());
      }
    }
  }
  return false;
}

bool search_extends_and_interfaces(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    std::set<const DexClass*>* visited,
    const DexClass* cls,
    const std::string& extends_class_name,
    const std::string& annotation) {
  assert(cls != nullptr);
  // Have we already visited this class? If yes, then the result is false.
  if (visited->find(cls) != visited->end()) {
    return false;
  }
  // Does this class match the annotation and type wildcard?
  if (type_and_annotation_match(
          regex_map, cls, extends_class_name, annotation)) {
    return true;
  }
  // Do any of the classes and interfaces above match?
  auto super_type = cls->get_super_class();
  if (super_type && super_type != get_object_type()) {
    auto super_class = type_class(super_type);
    if (super_class) {
      if (search_extends_and_interfaces(regex_map,
                                        visited,
                                        super_class,
                                        extends_class_name,
                                        annotation)) {
        return true;
      }
    } else {
      TRACE(PGR,
            8,
            "      WARNING: Can't find class for type %s\n",
            super_type->get_name()->c_str());
    }
  }
  // Do any of the interfaces from here and up match?
  bool found = search_interfaces(
      regex_map, visited, cls, extends_class_name, annotation);
  visited->emplace(cls);
  return found;
}

bool extends(std::unordered_map<std::string, boost::regex*>& regex_map,
             const DexClass* cls,
             const std::string& extends_class_name,
             const std::string& annotation) {
  if (extends_class_name.empty()) {
    return true;
  }
  auto deob_name = cls->get_deobfuscated_name();
  std::set<const DexClass*> visited;
  return search_extends_and_interfaces(
      regex_map, &visited, cls, extends_class_name, annotation);
}

bool classname_contains_wildcard(const std::string& classname) {
  for (char ch : classname) {
    if (ch == '*' || ch == '?' || ch == '!' || ch == '%' || ch == ',') {
      return true;
    }
  }
  return false;
}

bool class_level_match(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const KeepSpec& keep_rule,
    const DexClass* cls) {
  // Check for access match
  if (!access_matches(keep_rule.class_spec.setAccessFlags,
                      keep_rule.class_spec.unsetAccessFlags,
                      cls->get_access())) {
    return false;
  }
  // Check to see if an annotation guard needs to be matched.
  if (!(keep_rule.class_spec.annotationType.empty())) {
    if (!has_annotation(regex_map, cls, keep_rule.class_spec.annotationType)) {
      return false;
    }
  }
  // Check to see if an extends clause needs to be matched.
  return extends(regex_map,
                 cls,
                 keep_rule.class_spec.extendsClassName,
                 keep_rule.class_spec.extendsAnnotationType);
}

bool class_level_match(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const KeepSpec& keep_rule,
    const DexClass* cls,
    const boost::regex* matcher) {
  // Check for a class name match.
  auto deob_name = cls->get_deobfuscated_name();
  if (!boost::regex_match(deob_name, *matcher)) {
    return false;
  }
  return class_level_match(regex_map, keep_rule, cls);
}

DexClass* find_single_class(const ProguardMap& pg_map,
                            const std::string& descriptor) {
  if (classname_contains_wildcard(descriptor)) {
    return nullptr;
  }
  DexType* typ = DexType::get_type(pg_map.translate_class(descriptor).c_str());
  if (typ == nullptr) {
    return nullptr;
  }
  return type_class(typ);
}

std::vector<DexMethod*> all_method_matches(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const DexClass* cls,
    MemberSpecification& method_keep,
    const boost::regex* method_regex) {
  std::vector<DexMethod*> matches;
  for (const auto& method : cls->get_vmethods()) {
    if (method_level_match(regex_map, method_keep, method, method_regex)) {
      matches.push_back(method);
    }
  }
  for (const auto& method : cls->get_dmethods()) {
    if (method_level_match(regex_map, method_keep, method, method_regex)) {
      matches.push_back(method);
    }
  }
  return matches;
}

// Find all matching methods in class cls.
void matching_methods(std::unordered_map<std::string, boost::regex*>& regex_map,
                      std::vector<MemberSpecification>& method_keeps,
                      DexClass* cls,
                      const bool search_super_classes) {
  DexClass* class_to_search = cls;
  while (class_to_search != nullptr && !class_to_search->is_external()) {
    for (auto& method_keep : method_keeps) {
      auto qualified_method_regex = method_regex(method_keep);
      boost::regex* matcher =
          register_matcher(regex_map, qualified_method_regex);
      std::vector<DexMethod*> matched_methods =
          all_method_matches(regex_map, class_to_search, method_keep, matcher);
      if (matched_methods.empty()) {
        continue;
      }
      // Record a match for this method level keep rule.
      method_keep.mark_conditionally = true;
    }
    if (!search_super_classes) {
      break;
    }
    auto typ = class_to_search->get_super_class();
    if (typ == nullptr) {
      break;
    }
    class_to_search = type_class(typ);
  }
}

std::vector<DexField*> all_field_matches(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    const DexClass* cls,
    MemberSpecification& field_keep) {
  auto fieldtype_regex = field_regex(field_keep);
  auto ifields = cls->get_ifields();
  auto sfields = cls->get_sfields();
  std::vector<DexField*> matches;
  boost::regex* matcher = register_matcher(regex_map, fieldtype_regex);
  for (const auto& field : ifields) {
    if (field_level_match(regex_map, field_keep, field, matcher)) {
      matches.push_back(field);
    }
  }
  for (const auto& field : sfields) {
    if (field_level_match(regex_map, field_keep, field, matcher)) {
      matches.push_back(field);
    }
  }
  return matches;
}

// Find all matching fields in class cls.
void matching_fields(std::unordered_map<std::string, boost::regex*>& regex_map,
                     std::vector<MemberSpecification>& field_keeps,
                     DexClass* cls,
                     const bool search_super_classes) {
  DexClass* class_to_search = cls;
  while (class_to_search != nullptr && !class_to_search->is_external()) {
    for (auto& field_keep : field_keeps) {
      auto matched_fields =
          all_field_matches(regex_map, class_to_search, field_keep);
      if (matched_fields.empty()) {
        continue;
      }
      // Record a match for this field keep rule.
      field_keep.mark_conditionally = true;
    }
    if (!search_super_classes) {
      break;
    }
    auto typ = class_to_search->get_super_class();
    if (typ == nullptr) {
      break;
    }
    class_to_search = type_class(typ);
  }
}

bool all_conditionally_matched(std::vector<MemberSpecification> members) {
  if (members.empty()) {
    return true;
  }
  for (const auto& member : members) {
    if (!member.mark_conditionally) {
      return false;
    }
  }
  return true;
}

bool process_mark_conditionally(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    KeepSpec& keep_rule,
    DexClass* cls) {
  if (keep_rule.class_spec.fieldSpecifications.size() == 0 &&
      keep_rule.class_spec.methodSpecifications.size() == 0) {
    std::cerr << "WARNING: A keepclasseswithmembers rule for class "
              << keep_rule.class_spec.className
              << " has no field or member specifications.\n";
  }
  // Clear conditional marks.
  for (MemberSpecification& field_spec :
       keep_rule.class_spec.fieldSpecifications) {
    field_spec.mark_conditionally = false;
  }
  for (MemberSpecification& method_spec :
       keep_rule.class_spec.methodSpecifications) {
    method_spec.mark_conditionally = false;
  }
  matching_fields(
      regex_map, keep_rule.class_spec.fieldSpecifications, cls, false);
  matching_methods(
      regex_map, keep_rule.class_spec.methodSpecifications, cls, false);
  // Make sure every field and method keep rule is matched.
  return all_conditionally_matched(keep_rule.class_spec.fieldSpecifications) &&
         all_conditionally_matched(keep_rule.class_spec.methodSpecifications);
}

// Once a match has been made against a class i.e. the class name
// matches, the annotations match, the extends clause matches and the
// access modifier filters match, then start to apply the keep control
// bits to the class, members and appropriate classes and members
// in the class hierarchy.
void mark_class_and_members_for_keep(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    KeepSpec& keep_rule,
    DexClass* cls) {
  // First check to see if we need to mark conditionally to see if all
  // field and method rules match i.e. we habe a -keepclasseswithmembers
  // rule to process.
  if (keep_rule.mark_conditionally) {
    // If this class does not incur at least one match for each field
    // and method rule, then don't mark this class or its members.
    if (!process_mark_conditionally(regex_map, keep_rule, cls)) {
      return;
    }
  }
  // Mark descriptor classes
  if (keep_rule.includedescriptorclasses) {
    std::cout << "keep rule uses includedescriptorclasses which is not "
                 "implemented: "
              << redex::show_keep(keep_rule) << std::endl;
  }
  keep_rule.count++;
  if (keep_rule.mark_classes || keep_rule.mark_conditionally) {
    apply_keep_modifiers(keep_rule, cls);
    cls->rstate.set_keep();
    if (!keep_rule.allowobfuscation) {
      cls->rstate.increment_keep_count();
    }
    if (is_blanket_keep_rule(keep_rule)) {
      cls->rstate.set_blanket_keep();
    }
    // Mark non-empty <clinit> methods as seeds.
    keep_clinits(cls);
  }
  // Walk up the hierarhcy performig seed marking.
  DexClass* class_to_mark = cls;
  bool apply_modifiers = true;
  while (class_to_mark != nullptr && !class_to_mark->is_external()) {
    // Mark unconditionally.
    apply_field_keeps(regex_map, class_to_mark, keep_rule, apply_modifiers);
    apply_method_keeps(regex_map,
                       class_to_mark,
                       keep_rule,
                       apply_modifiers,
                       [](DexMethod* f) -> void { f->rstate.set_keep(); });
    apply_modifiers = false;
    auto typ = class_to_mark->get_super_class();
    if (typ == nullptr) {
      break;
    }
    class_to_mark = type_class(typ);
  }
}

void process_assumenosideeffects(
    std::unordered_map<std::string, boost::regex*>& regex_map,
    KeepSpec& keep_rule,
    DexClass* cls) {
  cls->rstate.set_assumenosideeffects();

  // Apply any method-level keep specifications.
  apply_method_keeps(
      regex_map, cls, keep_rule, false, [](DexMethod* m) -> void {
        m->rstate.set_assumenosideeffects();
      });
}

void process_keep(const ProguardMap& pg_map,
                  std::vector<KeepSpec>& keep_rules,
                  std::unordered_map<std::string, boost::regex*>& regex_map,
                  Scope& classes,
                  std::function<void(
                      std::unordered_map<std::string, boost::regex*>& regex_map,
                      KeepSpec&,
                      DexClass*)> keep_processor) {
  boost::regex* matcher;
  for (auto& keep_rule : keep_rules) {
    auto descriptor =
        proguard_parser::convert_wildcard_type(keep_rule.class_spec.className);
    DexClass* cls = find_single_class(pg_map, descriptor);
    if (cls != nullptr) {
      if (class_level_match(regex_map, keep_rule, cls)) {
        if (!cls->is_external()) {
          keep_processor(regex_map, keep_rule, cls);
        }
      }
      continue;
    }
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    matcher = register_matcher(regex_map, desc_regex);
    for (const auto& cls : classes) {
      // Skip external classes.
      if (cls->is_external()) {
        continue;
      }
      if (class_level_match(regex_map, keep_rule, cls, matcher)) {
        keep_processor(regex_map, keep_rule, cls);
      }
    }
  }
}

// TODO(satnamsingh@fb.com): Replace with a user defined hash function.
inline bool operator==(const MemberSpecification& lhs,
                       const MemberSpecification& rhs) {
  return lhs.requiredSetAccessFlags == rhs.requiredSetAccessFlags &&
         lhs.requiredUnsetAccessFlags == rhs.requiredUnsetAccessFlags &&
         lhs.annotationType == rhs.annotationType && lhs.name == rhs.name &&
         lhs.descriptor == rhs.descriptor;
}

inline bool operator==(const ClassSpecification& lhs,
                       const ClassSpecification& rhs) {
  return lhs.className == rhs.className &&
         lhs.annotationType == rhs.annotationType &&
         lhs.extendsClassName == rhs.extendsClassName &&
         lhs.extendsAnnotationType == rhs.extendsAnnotationType &&
         lhs.setAccessFlags == rhs.setAccessFlags &&
         lhs.unsetAccessFlags == rhs.unsetAccessFlags &&
         lhs.fieldSpecifications == rhs.fieldSpecifications &&
         lhs.methodSpecifications == rhs.methodSpecifications;
}

inline bool operator==(const KeepSpec& lhs, const KeepSpec& rhs) {
  return lhs.includedescriptorclasses == rhs.includedescriptorclasses &&
         lhs.allowshrinking == rhs.allowshrinking &&
         lhs.allowoptimization == rhs.allowoptimization &&
         lhs.allowobfuscation == rhs.allowobfuscation &&
         lhs.class_spec == rhs.class_spec;
}

void filter_duplicate_rules(std::vector<KeepSpec>* keep_rules) {
  std::vector<KeepSpec> unique;
  for (const auto& rule : *keep_rules) {
    auto it = std::find(unique.begin(), unique.end(), rule);
    if (it == unique.end()) {
      unique.push_back(rule);
    }
  }
  keep_rules->clear();
  for (const auto& rule : unique) {
    keep_rules->push_back(rule);
  }
}

void process_proguard_rules(const ProguardMap& pg_map,
                            ProguardConfiguration* pg_config,
                            Scope& classes) {
  size_t field_count = 0;
  size_t method_count = 0;
  for (const auto& cls : classes) {
    field_count += cls->get_ifields().size() + cls->get_sfields().size();
    method_count += cls->get_vmethods().size() + cls->get_dmethods().size();
  }
  std::unordered_map<std::string, boost::regex*> regex_map;
  // Filter out duplicate rules to speed up processing.
  filter_duplicate_rules(&pg_config->keep_rules);
  filter_duplicate_rules(&pg_config->assumenosideeffects_rules);
  // Now process each of the different kinds of rules as well
  // as assumenosideeffects.
  process_keep(pg_map,
               pg_config->keep_rules,
               regex_map,
               classes,
               mark_class_and_members_for_keep);
  process_keep(pg_map,
               pg_config->assumenosideeffects_rules,
               regex_map,
               classes,
               process_assumenosideeffects);
  for (auto& e : regex_map) {
    delete (e.second);
  }
  // By default, keep all annotation classes.
  for (auto cls : classes) {
    if (is_annotation(cls)) {
      cls->rstate.set_keep();
      cls->rstate.increment_keep_count();
    }
  }
}

} // namespace redex
