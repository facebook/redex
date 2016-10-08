/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/regex.hpp>
#include <iostream>
#include <set>
#include <string>

#include "DexAccess.h"
#include "DexUtil.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardRegex.h"
#include "keeprules.h"

namespace redex {

template <class DexMember>
void apply_keep_modifiers(const KeepSpec& k, DexMember* member) {
  if (k.includedescriptorclasses) {
    member->rstate.set_includedescriptorclasses();
  }
  if (k.allowoptimization) {
    member->rstate.set_allowoptimization();
  }
  if (k.allowshrinking) {
    member->rstate.set_allowshrinking();
  }
  if (k.allowobfuscation &&
      std::string(member->get_name()->c_str()) != "<init>") {
    member->rstate.set_allowobfuscation();
  }
}

bool check_required_access_flags(const std::set<AccessFlag>& requiredSet,
                                 const DexAccessFlags& access_flags) {
  auto require_public = requiredSet.count(AccessFlag::PUBLIC);
  auto require_private = requiredSet.count(AccessFlag::PRIVATE);
  auto require_protectd = requiredSet.count(AccessFlag::PROTECTED);
  bool match_one = require_public + require_private + require_protectd > 1;
  for (const AccessFlag& af : requiredSet) {
    switch (af) {
    case AccessFlag::PUBLIC:
      if (!(access_flags & ACC_PUBLIC)) {
        if (match_one) {
          if (require_private && (access_flags & ACC_PRIVATE)) {
            continue;
          }
          if (require_protectd && (access_flags & ACC_PROTECTED)) {
            continue;
          }
        }
        return false;
      }
      break;
    case AccessFlag::PRIVATE:
      if (!(access_flags & ACC_PRIVATE)) {
        if (match_one) {
          if (require_public && (access_flags & ACC_PUBLIC)) {
            continue;
          }
          if (require_protectd && (access_flags & ACC_PROTECTED)) {
            continue;
          }
        }
        return false;
      }
      break;
    case AccessFlag::PROTECTED:
      if (!(access_flags & ACC_PROTECTED)) {
        if (match_one) {
          if (require_public && (access_flags & ACC_PUBLIC)) {
            continue;
          }
          if (require_private && (access_flags & ACC_PRIVATE)) {
            continue;
          }
        }
        return false;
      }
      break;
    case AccessFlag::STATIC:
      if (!(access_flags & ACC_STATIC)) {
        return false;
      }
      break;
    case AccessFlag::FINAL:
      if (!(access_flags & ACC_FINAL)) {
        return false;
      }
      break;
    case AccessFlag::INTERFACE:
      if (!(access_flags & ACC_INTERFACE)) {
        return false;
      }
      break;
    case AccessFlag::SYNCHRONIZED:
      if (!(access_flags & ACC_SYNCHRONIZED)) {
        return false;
      }
      break;
    case AccessFlag::VOLATILE:
      if (!(access_flags & ACC_VOLATILE)) {
        return false;
      }
      break;
    case AccessFlag::TRANSIENT:
      if (!(access_flags & ACC_TRANSIENT)) {
        return false;
      }
      break;
    case AccessFlag::BRIDGE:
      if (!(access_flags & ACC_BRIDGE)) {
        return false;
      }
      break;
    case AccessFlag::VARARGS:
      if (!(access_flags & ACC_VARARGS)) {
        return false;
      }
      break;
    case AccessFlag::NATIVE:
      if (!(access_flags & ACC_NATIVE)) {
        return false;
      }
      break;
    case AccessFlag::ABSTRACT:
      if (!(access_flags & ACC_ABSTRACT)) {
        return false;
      }
      break;
    case AccessFlag::STRICT:
      if (!(access_flags & ACC_STRICT)) {
        return false;
      }
      break;
    case AccessFlag::SYNTHETIC:
      if (!(access_flags & ACC_SYNTHETIC)) {
        return false;
      }
      break;
    case AccessFlag::ANNOTATION:
      if (!(access_flags & ACC_ANNOTATION)) {
        return false;
      }
      break;
    case AccessFlag::ENUM:
      if (!(access_flags & ACC_ENUM)) {
        return false;
      }
      break;
    }
  }
  return true;
}

bool check_required_unset_access_flags(
    const std::set<AccessFlag>& requiredUnset,
    const DexAccessFlags& access_flags) {
  for (const AccessFlag& af : requiredUnset) {
    switch (af) {
    case AccessFlag::PUBLIC:
      if ((access_flags & ACC_PUBLIC)) {
        return false;
      }
      break;
    case AccessFlag::PRIVATE:
      if ((access_flags & ACC_PRIVATE)) {
        return false;
      }
      break;
    case AccessFlag::PROTECTED:
      if ((access_flags & ACC_PROTECTED)) {
        return false;
      }
      break;
    case AccessFlag::STATIC:
      if ((access_flags & ACC_STATIC)) {
        return false;
      }
      break;
    case AccessFlag::FINAL:
      if ((access_flags & ACC_FINAL)) {
        return false;
      }
      break;
    case AccessFlag::INTERFACE:
      if ((access_flags & ACC_INTERFACE)) {
        return false;
      }
      break;
    case AccessFlag::SYNCHRONIZED:
      if ((access_flags & ACC_SYNCHRONIZED)) {
        return false;
      }
      break;
    case AccessFlag::VOLATILE:
      if ((access_flags & ACC_VOLATILE)) {
        return false;
      }
      break;
    case AccessFlag::TRANSIENT:
      if ((access_flags & ACC_TRANSIENT)) {
        return false;
      }
      break;
    case AccessFlag::BRIDGE:
      if ((access_flags & ACC_BRIDGE)) {
        return false;
      }
      break;
    case AccessFlag::VARARGS:
      if ((access_flags & ACC_VARARGS)) {
        return false;
      }
      break;
    case AccessFlag::NATIVE:
      if ((access_flags & ACC_NATIVE)) {
        return false;
      }
      break;
    case AccessFlag::ABSTRACT:
      if ((access_flags & ACC_ABSTRACT)) {
        return false;
      }
      break;
    case AccessFlag::STRICT:
      if ((access_flags & ACC_STRICT)) {
        return false;
      }
      break;
    case AccessFlag::SYNTHETIC:
      if ((access_flags & ACC_SYNTHETIC)) {
        return false;
      }
      break;
    case AccessFlag::ANNOTATION:
      if ((access_flags & ACC_ANNOTATION)) {
        return false;
      }
      break;
    case AccessFlag::ENUM:
      if ((access_flags & ACC_ENUM)) {
        return false;
      }
      break;
    }
  }
  return true;
}

template <class DexMember>
bool has_annotation(const DexMember* member, const std::string& annotation) {
  auto annos = member->get_anno_set();
  if (annos != nullptr) {
    auto annotation_regex = proguard_parser::convert_wildcard_type(annotation);
    boost::regex annotation_matcher(annotation_regex);
    for (const auto& anno : annos->get_annotations()) {
      if (boost::regex_match(anno->type()->c_str(), annotation_matcher)) {
        return true;
      }
    }
  }
  return false;
}

bool access_matches(const std::set<AccessFlag>& requiredSet,
                    const std::set<AccessFlag>& requiredUnset,
                    const DexAccessFlags& access_flags) {
  return check_required_access_flags(requiredSet, access_flags) &&
         check_required_unset_access_flags(requiredUnset, access_flags);
}

// From a fully qualified descriptor for a field, exract just the
// name of the field which occurs between the ;. and : characters.
std::string extract_fieldname(std::string qualified_fieldname) {
  auto p = qualified_fieldname.find(";.");
  auto e = qualified_fieldname.find(":");
  return qualified_fieldname.substr(p + 2, e - p - 2);
}

bool field_level_match(const redex::MemberSpecification& fieldSpecification,
                       DexField* field,
                       const boost::regex& fieldname_regex) {
  // Check for annotation guards.
  if (fieldSpecification.annotationType != "") {
    if (!has_annotation(field, fieldSpecification.annotationType)) {
      return false;
    }
  }
  // Match field name against regex.
  auto pg_name = proguard_name(field);
  auto qualified_name = field->get_deobfuscated_name();
  auto field_name = extract_fieldname(qualified_name);
  return boost::regex_match(field_name, fieldname_regex);
}

void keep_fields(const std::list<DexField*>& fields,
                 const redex::KeepSpec& keep_rule,
                 std::function<void(DexField*)> keeper) {
  auto fieldSpecifications = keep_rule.class_spec.fieldSpecifications;
  for (auto field : fields) {
    for (const auto& fieldSpecification : fieldSpecifications) {
      boost::regex fieldname_regex(
          proguard_parser::form_member_regex(fieldSpecification.name));
      if (field_level_match(fieldSpecification, field, fieldname_regex)) {
        keeper(field);
        apply_keep_modifiers(keep_rule, field);
      }
    }
  }
}

void apply_field_keeps(const DexClass* cls,
                       const redex::KeepSpec& keep_rule,
                       std::function<void(DexField*)> keeper) {
  keep_fields(cls->get_ifields(), keep_rule, keeper);
  keep_fields(cls->get_sfields(), keep_rule, keeper);
}

bool method_level_match(const redex::MemberSpecification& methodSpecification,
                        DexMethod* method,
                        const boost::regex& method_regex) {
  // Check to see if the method match is guarded by an annotaiton match.
  if (methodSpecification.annotationType != "") {
    if (!has_annotation(method, methodSpecification.annotationType)) {
      return false;
    }
  }
  auto qualified_name = method->get_deobfuscated_name();
  return boost::regex_match(qualified_name.c_str(), method_regex);
}

void keep_methods(const redex::KeepSpec& keep_rule,
                  const redex::MemberSpecification& methodSpecification,
                  const std::list<DexMethod*>& methods,
                  const boost::regex& method_regex,
                  std::function<void(DexMethod*)> keeper) {
  for (const auto& method : methods) {
    if (method_level_match(methodSpecification, method, method_regex)) {
      keeper(method);
      apply_keep_modifiers(keep_rule, method);
    }
  }
}

void apply_method_keeps(const DexClass* cls,
                        const redex::KeepSpec& keep_rule,
                        std::function<void(DexMethod*)> keeper) {
  auto classname = keep_rule.class_spec.className;
  auto methodSpecifications = keep_rule.class_spec.methodSpecifications;
  for (const auto& method_spec : methodSpecifications) {
    auto descriptor = proguard_parser::convert_wildcard_type(classname);
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    auto qualified_method_name =
        desc_regex + "\\." +
        proguard_parser::form_member_regex(method_spec.name) + "\\:" +
        proguard_parser::form_type_regex(method_spec.descriptor);
    TRACE(PGR,
          8,
          "====> Method match against regex method: %s.%s:%s\n",
          classname.c_str(),
          method_spec.name.c_str(),
          method_spec.descriptor.c_str());
    TRACE(PGR, 8, "====> Using regex %s\n", qualified_method_name.c_str());
    boost::regex method_regex(qualified_method_name);
    keep_methods(keep_rule, method_spec, cls->get_vmethods(), method_regex,
                 keeper);
    keep_methods(keep_rule, method_spec, cls->get_dmethods(), method_regex,
                 keeper);
  }
}

// This function checks to see of a class satisfies a match
// against an annotation type and a class wildcard.
bool type_and_annotation_match(const DexClass* cls,
                               const std::string& extends_class_name,
                               const std::string& annotation) {
  if (cls == nullptr) {
    return false;
  }
  if (cls->get_type() == get_object_type()) {
    return false;
  }
  // First check to see if an annotation type needs to be matched.
  if (annotation != "") {
    if (!has_annotation(cls, annotation)) {
      return false;
    }
  }
  // Now try to match against the class name.
  auto deob_name = cls->get_deobfuscated_name();
  auto descriptor = proguard_parser::convert_wildcard_type(extends_class_name);
  auto desc_regex = proguard_parser::form_type_regex(descriptor);
  boost::regex matcher(desc_regex);
  bool matched = boost::regex_match(deob_name, matcher);
  return matched;
}

bool search_extends_and_interfaces(std::set<const DexClass*>* visited,
                                   const DexClass* cls,
                                   const std::string& extends_class_name,
                                   const std::string& annotation);

bool search_interfaces(std::set<const DexClass*>* visited,
                       const DexClass* cls,
                       const std::string& name,
                       const std::string& annotation) {
  auto interfaces = cls->get_interfaces();
  if (interfaces) {
    for (const auto& impl : interfaces->get_type_list()) {
      auto impl_class = type_class(impl);
      if (impl_class) {
        if (search_extends_and_interfaces(
                visited, impl_class, name, annotation)) {
          return true;
        }
      } else {
        TRACE(PGR, 8, "WARNING: Can't find class for type %s\n", impl->c_str());
      }
    }
  }
  return false;
}

bool search_extends_and_interfaces(std::set<const DexClass*>* visited,
                                   const DexClass* cls,
                                   const std::string& extends_class_name,
                                   const std::string& annotation) {
  assert(cls != nullptr);
  // Have we already visited this class? If yes, then the result is false.
  if (visited->find(cls) != visited->end()) {
    return false;
  }
  // Does this class match the annotation and type wildcard?
  if (type_and_annotation_match(cls, extends_class_name, annotation)) {
    return true;
  }
  // Do any of the classes and interface above match?
  auto super_type = cls->get_super_class();
  if (super_type && super_type != get_object_type()) {
    auto super_class = type_class(super_type);
    if (super_class) {
      if (search_extends_and_interfaces(
              visited, super_class, extends_class_name, annotation)) {
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
  bool found = search_interfaces(visited, cls, extends_class_name, annotation);
  visited->emplace(cls);
  return found;
}

bool extends(const DexClass* cls,
             const std::string& extends_class_name,
             const std::string& annotation) {
  if (extends_class_name == "") {
    return true;
  }
  auto deob_name = cls->get_deobfuscated_name();
  std::set<const DexClass*> visited;
  return search_extends_and_interfaces(
      &visited, cls, extends_class_name, annotation);
}

bool classname_contains_wildcard(const std::string& classname) {
  for (char ch : classname) {
    if (ch == '*' || ch == '?') {
      return true;
    }
  }
  return false;
}

bool class_level_match(const KeepSpec& keep_rule, const DexClass* cls) {
  // Check for access match
  if (!access_matches(keep_rule.class_spec.setAccessFlags,
                      keep_rule.class_spec.unsetAccessFlags,
                      cls->get_access())) {
    return false;
  }
  // Check to see if an annotation guard needs to be matched.
  if (keep_rule.class_spec.annotationType != "") {
    if (!has_annotation(cls, keep_rule.class_spec.annotationType)) {
      return false;
    }
  }
  // Check to see if an extends clause needs to be matched.
  return extends(cls,
                 keep_rule.class_spec.extendsClassName,
                 keep_rule.class_spec.extendsAnnotationType);
}

bool class_level_match(const KeepSpec& keep_rule,
                       const DexClass* cls,
                       const boost::regex& matcher) {
  // Check for a class name match.
  auto deob_name = cls->get_deobfuscated_name();
  if (!boost::regex_match(deob_name, matcher)) {
    return false;
  }
  return class_level_match(keep_rule, cls);
}

void mark_class_and_members_for_keep(const KeepSpec& keep_rule,
                                     DexClass* cls) {
  if (cls->is_external()) {
    return;
  }
  cls->rstate.set_keep();
  // Apply the keep option modifiers.
  apply_keep_modifiers(keep_rule, cls);
  // Apply any field-level keep specifications.
  apply_field_keeps(cls, keep_rule,
                    [](DexField* f) -> void { f->rstate.set_keep(); });
  // Apply any method-level keep specifications.
  apply_method_keeps(cls, keep_rule,
                    [](DexMethod* m) -> void { m->rstate.set_keep(); });
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

bool method_matches(const MemberSpecification& method_keep,
                    const std::list<DexMethod*>& methods,
                    const boost::regex& method_regex) {
  for (const auto& method : methods) {
    if (method_level_match(method_keep, method, method_regex)) {
      return true;
    }
  }
  return false;
}

bool all_methods_match(const std::string& classname,
                       const std::vector<MemberSpecification>& method_keeps,
                       const std::list<DexMethod*>& methods) {
  if (methods.empty()) {
    return true;
  }
  for (const auto& method_keep : method_keeps) {
    auto descriptor = proguard_parser::convert_wildcard_type(classname);
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    auto qualified_method_name =
        desc_regex + "\\." +
        proguard_parser::form_member_regex(method_keep.name) + ":" +
        proguard_parser::form_type_regex(method_keep.descriptor);
    boost::regex method_regex(qualified_method_name);
    if (!method_matches(method_keep, methods, method_regex)) {
      return false;
    }
  }
  return true;
}

bool field_matches(const MemberSpecification& field_keep,
                   const std::list<DexField*>& fields) {
  boost::regex fieldname_regex(
      proguard_parser::form_member_regex(field_keep.name));
  for (const auto& field : fields) {
    if (field_level_match(field_keep, field, fieldname_regex)) {
      return true;
    }
  }
  return false;
}

bool all_fields_match(const std::vector<MemberSpecification>& field_keeps,
                      const std::list<DexField*>& fields) {
  if (field_keeps.empty()) {
    return true;
  }
  for (const auto& field_keep : field_keeps) {
    if (!field_matches(field_keep, fields)) {
      return false;
    }
  }
  return true;
}

void process_keepclasseswithmembers(const KeepSpec& keep_rule, DexClass* cls) {
  if (all_fields_match(keep_rule.class_spec.fieldSpecifications,
                       cls->get_ifields()) &&
      all_fields_match(keep_rule.class_spec.fieldSpecifications,
                       cls->get_sfields()) &&
      all_methods_match(keep_rule.class_spec.className,
                        keep_rule.class_spec.methodSpecifications,
                        cls->get_vmethods()) &&
      all_methods_match(keep_rule.class_spec.className,
                        keep_rule.class_spec.methodSpecifications,
                        cls->get_dmethods())) {
    mark_class_and_members_for_keep(keep_rule, cls);
  }
}

void process_keepclassmembers(const KeepSpec& keep_rule, DexClass* cls) {
  if (cls->is_external()) {
    return;
  }
  cls->rstate.set_keepclassmembers();
  // Apply the keep option modifiers.
  apply_keep_modifiers(keep_rule, cls);
  // Apply any field-level keep specifications.
  apply_field_keeps(cls, keep_rule,
                    [](DexField* f) -> void { f->rstate.set_keepclassmembers(); });
  // Apply any method-level keep specifications.
  apply_method_keeps(cls, keep_rule,
                    [](DexMethod* m) -> void { m->rstate.set_keepclassmembers(); });
}

void process_assumenosideeffects(const KeepSpec& keep_rule, DexClass* cls) {
  cls->rstate.set_assumenosideeffects();
  // Apply the keep option modifiers.
  apply_keep_modifiers(keep_rule, cls);
  // Apply any field-level keep specifications.
  apply_field_keeps(cls, keep_rule,
                    [](DexField* f) -> void { f->rstate.set_assumenosideeffects(); });
  // Apply any method-level keep specifications.
  apply_method_keeps(cls, keep_rule,
                    [](DexMethod* m) -> void { m->rstate.set_assumenosideeffects(); });
}

void process_keep(const ProguardMap& pg_map,
                  const std::vector<KeepSpec>& keep_rules,
                  Scope& classes,
                  std::function<void(KeepSpec, DexClass*)> keep_processor) {
  for (const auto& keep_rule : keep_rules) {
    auto descriptor =
        proguard_parser::convert_wildcard_type(keep_rule.class_spec.className);
    DexClass* cls = find_single_class(pg_map, descriptor);
    if (cls != nullptr) {
      if (class_level_match(keep_rule, cls)) {
        keep_processor(keep_rule, cls);
      }
      continue;
    }
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(desc_regex);
    for (const auto& cls : classes) {
      if (class_level_match(keep_rule, cls, matcher)) {
        keep_processor(keep_rule, cls);
      }
    }
  }
}

void process_proguard_rules(const ProguardMap& pg_map,
                            const ProguardConfiguration& pg_config,
                            Scope& classes) {
  process_keep(pg_map,
               pg_config.keep_rules,
               classes,
               mark_class_and_members_for_keep);
  process_keep(pg_map,
               pg_config.keepclasseswithmembers_rules,
               classes,
               process_keepclasseswithmembers);
  process_keep(pg_map,
               pg_config.keepclassmembers_rules,
               classes,
               process_keepclassmembers);
  process_keep(pg_map,
               pg_config.assumesideeffects_rules,
               classes,
               process_assumenosideeffects);
}

} // namespace redex
