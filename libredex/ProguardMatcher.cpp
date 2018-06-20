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
#include <thread>

#include "ClassHierarchy.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "ProguardMatcher.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardRegex.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Timer.h"
#include "WorkQueue.h"

namespace redex {

namespace {

using RegexMap = std::unordered_map<std::string, boost::regex>;

std::unique_ptr<boost::regex> make_rx(const std::string& s,
                                      bool convert = true) {
  if (s.empty()) return nullptr;
  auto wc = convert ? proguard_parser::convert_wildcard_type(s) : s;
  auto rx = proguard_parser::form_type_regex(wc);
  return std::make_unique<boost::regex>(rx);
}

bool match_annotation_rx(const DexClass* cls, const boost::regex& annorx) {
  const auto* annos = cls->get_anno_set();
  if (!annos) return false;
  for (const auto& anno : annos->get_annotations()) {
    if (boost::regex_match(anno->type()->c_str(), annorx)) {
      return true;
    }
  }
  return false;
}

/**
 * Helper class that holds the conditions for a class-level match on a keep
 * rule.
 */
struct ClassMatcher {
  explicit ClassMatcher(const KeepSpec& ks)
      : setFlags_(ks.class_spec.setAccessFlags),
        unsetFlags_(ks.class_spec.unsetAccessFlags),
        m_class_name(ks.class_spec.className),
        m_cls(make_rx(ks.class_spec.className)),
        m_anno(make_rx(ks.class_spec.annotationType, false)),
        m_extends(make_rx(ks.class_spec.extendsClassName)),
        m_extends_anno(make_rx(ks.class_spec.extendsAnnotationType, false)) {}

  bool match(const DexClass* cls) {
    // Check for class name match
    // `match_name` is really slow; let's short-circuit it for wildcard-only
    // matches
    if (m_class_name != "*" && m_class_name != "**" && !match_name(cls)) {
      return false;
    }
    // Check for access match
    if (!match_access(cls)) {
      return false;
    }
    // Check to see if an annotation guard needs to be matched.
    if (!match_annotation(cls)) {
      return false;
    }
    // Check to see if an extends clause needs to be matched.
    return match_extends(cls);
  }

 private:
  bool match_name(const DexClass* cls) const {
    const auto& deob_name = cls->get_deobfuscated_name();
    return boost::regex_match(deob_name, *m_cls);
  }

  bool match_access(const DexClass* cls) const {
    return access_matches(setFlags_, unsetFlags_, cls->get_access());
  }

  bool match_annotation(const DexClass* cls) const {
    if (!m_anno) return true;
    return match_annotation_rx(cls, *m_anno);
  }

  bool match_extends(const DexClass* cls) {
    if (!m_extends) return true;
    return search_extends_and_interfaces(cls);
  }

  bool type_and_annotation_match(const DexClass* cls) const {
    if (cls == nullptr) return false;
    if (cls->get_type() == get_object_type()) return false;
    // First check to see if an annotation type needs to be matched.
    if (m_extends_anno) {
      if (!match_annotation_rx(cls, *m_extends_anno)) {
        return false;
      }
    }
    const auto& deob_name = cls->get_deobfuscated_name();
    return boost::regex_match(deob_name, *m_extends);
  }

  bool search_interfaces(const DexClass* cls) {
    const auto* interfaces = cls->get_interfaces();
    if (!interfaces) return false;
    for (const auto& impl : interfaces->get_type_list()) {
      auto impl_class = type_class(impl);
      if (impl_class) {
        if (search_extends_and_interfaces(impl_class)) {
          return true;
        }
      }
    }
    return false;
  }

  bool search_extends_and_interfaces(const DexClass* cls) {
    auto cached_it = m_extends_result_cache.find(cls);
    if (cached_it != m_extends_result_cache.end()) {
      return cached_it->second;
    }
    auto result = search_extends_and_interfaces_nocache(cls);
    m_extends_result_cache.emplace(cls, result);
    return result;
  }

  bool search_extends_and_interfaces_nocache(const DexClass* cls) {
    always_assert(cls != nullptr);
    // Does this class match the annotation and type wildcard?
    if (type_and_annotation_match(cls)) {
      return true;
    }
    // Do any of the classes and interfaces above match?
    auto super_type = cls->get_super_class();
    if (super_type && super_type != get_object_type()) {
      auto super_class = type_class(super_type);
      if (super_class) {
        if (search_extends_and_interfaces(super_class)) {
          return true;
        }
      }
    }
    // Do any of the interfaces from here and up match?
    return search_interfaces(cls);
  }

  DexAccessFlags setFlags_;
  DexAccessFlags unsetFlags_;
  std::string m_class_name;
  std::unique_ptr<boost::regex> m_cls;
  std::unique_ptr<boost::regex> m_anno;
  std::unique_ptr<boost::regex> m_extends;
  std::unique_ptr<boost::regex> m_extends_anno;

  std::unordered_map<const DexClass*, bool> m_extends_result_cache;
};

} // namespace

// Updates a class, field or method to add keep modifiers.
// Note: includedescriptorclasses and allowoptimization are not implemented.
template <class DexMember>
void apply_keep_modifiers(const KeepSpec& k, DexMember* member) {
  // We only set allowshrinking when no other keep rule has been applied to this
  // class or member.
  //
  // Note that multiple keep rules could set or unset the modifier
  // *conflictingly*. It would be best if all the keep rules are never
  // contradictory each other. But verifying the integrity takes some time, and
  // programmers must fix the rules. Instead, we pick a conservative choice:
  // don't shrink or don't obfuscate.
  if (k.allowshrinking) {
    if (!keep(member)) {
      member->rstate.set_allowshrinking();
    } else {
      // We already observed a keep rule for this member. So, even if another
      // "-keep,allowshrinking" tries to set allowshrinking, we must ignore it.
    }
  } else {
    // Otherwise reset it: don't allow shrinking.
    member->rstate.unset_allowshrinking();
  }
  // The same case: unsetting allowobfuscation has a priority.
  if (k.allowobfuscation) {
    if (!keep(member) && strcmp(member->get_name()->c_str(), "<init>") != 0) {
      member->rstate.set_allowobfuscation();
    }
  } else {
    member->rstate.unset_allowobfuscation();
  }
}

// Is this keep_rule an application of a blanket top-level keep
// "-keep,allowshrinking class *" or "-keepnames class *" rule?
// See keepclassnames.pro, or T1890454.
inline bool is_blanket_keepnames_rule(const KeepSpec& keep_rule) {
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

boost::regex& register_matcher(RegexMap& regex_map, const std::string& regex) {
  auto where = regex_map.find(regex);
  if (where == regex_map.end()) {
    regex_map.emplace(regex, boost::regex{regex});
    return regex_map.at(regex);
  }
  return where->second;
}

template <class DexMember>
bool has_annotation(RegexMap& regex_map,
                    const DexMember* member,
                    const std::string& annotation) {
  auto annos = member->get_anno_set();
  if (annos != nullptr) {
    auto annotation_regex = proguard_parser::form_type_regex(annotation);
    const boost::regex& annotation_matcher =
        register_matcher(regex_map, annotation_regex);
    for (const auto& anno : annos->get_annotations()) {
      if (boost::regex_match(anno->type()->c_str(), annotation_matcher)) {
        return true;
      }
    }
  }
  return false;
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

bool field_level_match(RegexMap& regex_map,
                       const redex::MemberSpecification& fieldSpecification,
                       const DexField* field,
                       const boost::regex& fieldname_regex) {
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
  return boost::regex_match(dequalified_name, fieldname_regex);
}

template <class Container>
void keep_fields(RegexMap& regex_map,
                 const redex::KeepSpec& keep_rule,
                 bool apply_modifiers,
                 const Container& fields,
                 const redex::MemberSpecification& fieldSpecification,
                 const std::function<void(DexField*)>& keeper,
                 const boost::regex& fieldname_regex) {
  for (DexField* field : fields) {
    if (!field_level_match(
            regex_map, fieldSpecification, field, fieldname_regex)) {
      continue;
    }
    if (apply_modifiers) {
      apply_keep_modifiers(keep_rule, field);
    }
    keeper(field);
    if (field->rstate.report_whyareyoukeeping()) {
      TRACE(PGR,
            2,
            "whyareyoukeeping Field %s kept by %s\n",
            SHOW(field),
            show_keep(keep_rule).c_str());
    }
    fieldSpecification.count++;
  }
}

std::string field_regex(const MemberSpecification& field_spec) {
  std::ostringstream ss;
  ss << proguard_parser::form_member_regex(field_spec.name);
  ss << "\\:";
  ss << proguard_parser::form_type_regex(field_spec.descriptor);
  return ss.str();
}

void apply_field_keeps(RegexMap& regex_map,
                       const DexClass* cls,
                       const redex::KeepSpec& keep_rule,
                       bool apply_modifiers,
                       const std::function<void(DexField*)>& keeper) {
  for (const auto& field_spec : keep_rule.class_spec.fieldSpecifications) {
    auto fieldname_regex = field_regex(field_spec);
    const boost::regex& matcher = register_matcher(regex_map, fieldname_regex);
    keep_fields(regex_map,
                keep_rule,
                apply_modifiers,
                cls->get_ifields(),
                field_spec,
                keeper,
                matcher);
    keep_fields(regex_map,
                keep_rule,
                apply_modifiers,
                cls->get_sfields(),
                field_spec,
                keeper,
                matcher);
  }
}

bool method_level_match(RegexMap& regex_map,
                        const redex::MemberSpecification& methodSpecification,
                        const DexMethod* method,
                        const boost::regex& method_regex) {
  // Check to see if the method match is guarded by an annotation match.
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
  return boost::regex_match(dequalified_name.c_str(), method_regex);
}

void keep_clinits(DexClass* cls) {
  for (auto method : cls->get_dmethods()) {
    if (is_clinit(method) && method->get_code()) {
      auto ii = InstructionIterable(method->get_code());
      auto it = ii.begin();
      while (opcode::is_load_param(it->insn->opcode())) {
        ++it;
      }
      if (!(it->insn->opcode() == OPCODE_RETURN_VOID && (++it) == ii.end())) {
        method->rstate.set_keep();
      }
      break;
    }
  }
}

template <class Container>
void keep_methods(RegexMap& regex_map,
                  const KeepSpec& keep_rule,
                  bool apply_modifiers,
                  const redex::MemberSpecification& methodSpecification,
                  const Container& methods,
                  const boost::regex& method_regex,
                  const std::function<void(DexMethod*)>& keeper) {
  for (DexMethod* method : methods) {
    if (method_level_match(
            regex_map, methodSpecification, method, method_regex)) {
      if (apply_modifiers) {
        apply_keep_modifiers(keep_rule, method);
      }
      keeper(method);
      if (method->rstate.report_whyareyoukeeping()) {
        TRACE(PGR,
              2,
              "whyareyoukeeping Method %s kept by %s\n",
              SHOW(method),
              show_keep(keep_rule).c_str());
      }
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

void apply_method_keeps(RegexMap& regex_map,
                        const DexClass* cls,
                        const redex::KeepSpec& keep_rule,
                        bool apply_modifiers,
                        const std::function<void(DexMethod*)>& keeper) {
  auto methodSpecifications = keep_rule.class_spec.methodSpecifications;
  for (auto& method_spec : methodSpecifications) {
    auto qualified_method_regex = method_regex(method_spec);
    const boost::regex& method_regex =
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

bool classname_contains_wildcard(const std::string& classname) {
  for (char ch : classname) {
    if (ch == '*' || ch == '?' || ch == '!' || ch == '%' || ch == ',') {
      return true;
    }
  }
  return false;
}

DexClass* find_single_class(const ProguardMap& pg_map,
                            const std::string& descriptor) {
  auto const& dsc = JavaNameUtil::external_to_internal(descriptor);
  DexType* typ = DexType::get_type(pg_map.translate_class(dsc).c_str());
  if (typ == nullptr) {
    typ = DexType::get_type(dsc.c_str());
    if (typ == nullptr) {
      return nullptr;
    }
  }
  return type_class(typ);
}

std::vector<DexMethod*> all_method_matches(
    RegexMap& regex_map,
    const DexClass* cls,
    const MemberSpecification& method_keep,
    const boost::regex& method_regex) {
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
void matching_methods(RegexMap& regex_map,
                      std::vector<MemberSpecification>& method_keeps,
                      const DexClass* cls,
                      bool search_super_classes) {
  const DexClass* class_to_search = cls;
  while (class_to_search != nullptr && !class_to_search->is_external()) {
    for (auto& method_keep : method_keeps) {
      auto qualified_method_regex = method_regex(method_keep);
      const boost::regex& matcher =
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
    RegexMap& regex_map,
    const DexClass* cls,
    const MemberSpecification& field_keep) {
  auto fieldtype_regex = field_regex(field_keep);
  auto ifields = cls->get_ifields();
  auto sfields = cls->get_sfields();
  std::vector<DexField*> matches;
  const boost::regex& matcher = register_matcher(regex_map, fieldtype_regex);
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
void matching_fields(RegexMap& regex_map,
                     std::vector<MemberSpecification>& field_keeps,
                     const DexClass* cls,
                     bool search_super_classes) {
  const DexClass* class_to_search = cls;
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

bool all_conditionally_matched(
    const std::vector<MemberSpecification>& members) {
  return std::all_of(
      members.begin(), members.end(), [](const MemberSpecification& m) {
        return m.mark_conditionally;
      });
}

bool process_mark_conditionally(RegexMap& regex_map,
                                KeepSpec& keep_rule,
                                const DexClass* cls) {
  if (keep_rule.class_spec.fieldSpecifications.empty() &&
      keep_rule.class_spec.methodSpecifications.empty()) {
    std::cerr << "WARNING: A keepclasseswithmembers rule for class "
              << keep_rule.class_spec.className
              << " has no field or member specifications.\n";
  }
  // Clear conditional marks.
  for (auto& field_spec : keep_rule.class_spec.fieldSpecifications) {
    field_spec.mark_conditionally = false;
  }
  for (auto& method_spec : keep_rule.class_spec.methodSpecifications) {
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
//
// Parallelization note: We parallelize process_keep, and this function will be
// eventually executed concurrently. There are potential races in rstate:
// (1) m_keep, (2) m_(un)set_allow(shrinking|obfuscation), (3)
// m_blanket_keepnames, and (4) m_keep_count. We use an atomic value for
// m_keep_count, but the other boolean values are always overwritten. These WAW
// (write-after-write) races are benign and do not affect the results.
void mark_class_and_members_for_keep(RegexMap& regex_map,
                                     KeepSpec& keep_rule,
                                     DexClass* cls) {
  // First check to see if we need to mark conditionally to see if all
  // field and method rules match i.e. we have a -keepclasseswithmembers
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
    std::cerr << "WARNING: 'includedescriptorclasses' keep modifier is NOT "
                 "implemented: "
              << redex::show_keep(keep_rule) << std::endl;
  }
  if (keep_rule.allowoptimization) {
    std::cerr
        << "WARNING: 'allowoptimization' keep modifier is NOT implemented: "
        << redex::show_keep(keep_rule) << std::endl;
  }
  keep_rule.count++;
  if (keep_rule.mark_classes || keep_rule.mark_conditionally) {
    apply_keep_modifiers(keep_rule, cls);
    cls->rstate.set_keep();
    if (cls->rstate.report_whyareyoukeeping()) {
      TRACE(
          PGR,
          2,
          "whyareyoukeeping Class %s kept by %s\n",
          redex::dexdump_name_to_dot_name(cls->get_deobfuscated_name()).c_str(),
          show_keep(keep_rule).c_str());
    }
    if (!keep_rule.allowobfuscation) {
      cls->rstate.increment_keep_count();
    }
    if (is_blanket_keepnames_rule(keep_rule)) {
      cls->rstate.set_blanket_keepnames();
    }
    // Mark non-empty <clinit> methods as seeds.
    keep_clinits(cls);
  }
  // Walk up the hierarchy performing seed marking.
  DexClass* class_to_mark = cls;
  bool apply_modifiers = true;
  while (class_to_mark != nullptr && !class_to_mark->is_external()) {
    // Mark unconditionally.
    apply_field_keeps(
        regex_map, class_to_mark, keep_rule, apply_modifiers, [](DexField* f) {
          f->rstate.set_keep();
        });
    apply_method_keeps(
        regex_map, class_to_mark, keep_rule, apply_modifiers, [](DexMethod* m) {
          m->rstate.set_keep();
        });
    apply_modifiers = false;
    auto typ = class_to_mark->get_super_class();
    if (typ == nullptr) {
      break;
    }
    class_to_mark = type_class(typ);
  }
}

// This function is also executed concurrently.
void process_whyareyoukeeping(RegexMap& regex_map,
                              KeepSpec& keep_rule,
                              DexClass* cls) {
  cls->rstate.set_whyareyoukeeping();

  apply_field_keeps(regex_map, cls, keep_rule, false, [](DexField* f) {
    f->rstate.set_whyareyoukeeping();
  });
  // Set any method-level keep whyareyoukeeping bits.
  apply_method_keeps(regex_map, cls, keep_rule, false, [](DexMethod* m) {
    m->rstate.set_whyareyoukeeping();
  });
}

// This function is also executed concurrently.
void process_assumenosideeffects(RegexMap& regex_map,
                                 KeepSpec& keep_rule,
                                 DexClass* cls) {
  cls->rstate.set_assumenosideeffects();

  // Apply any method-level keep specifications.
  apply_method_keeps(regex_map, cls, keep_rule, false, [](DexMethod* m) {
    m->rstate.set_assumenosideeffects();
  });
}

/*
 * Build a DAG of class -> subclass and implementors. This is fairly similar to
 * build_type_hierarchy and friends, but Proguard doesn't distinguish between
 * subclasses and interface implementors, so this function combines them
 * together.
 */
void build_extends_or_implements_hierarchy(const Scope& scope,
                                           ClassHierarchy* hierarchy) {
  for (const auto& cls : scope) {
    const auto* type = cls->get_type();
    // ensure an entry for the DexClass is created
    (*hierarchy)[type];
    const auto* super = cls->get_super_class();
    if (super != nullptr) {
      (*hierarchy)[super].insert(type);
    }
    for (const auto& impl : cls->get_interfaces()->get_type_list()) {
      (*hierarchy)[impl].insert(type);
    }
  }
}

void process_keep(
    const ProguardMap& pg_map,
    std::vector<KeepSpec>& keep_rules,
    const Scope& classes,
    const Scope& external_classes,
    const ClassHierarchy& hierarchy,
    std::function<void(RegexMap&, KeepSpec&, DexClass*)> keep_processor,
    const std::string& name) {
  Timer t("Process keep for " + name);

  auto process_single_keep = [&keep_processor](ClassMatcher& class_match,
                                               KeepSpec& keep_rule,
                                               DexClass* cls,
                                               RegexMap& regex_map) {
    // Skip external classes.
    if (cls == nullptr || cls->is_external()) {
      return;
    }
    if (class_match.match(cls)) {
      keep_processor(regex_map, keep_rule, cls);
    }
  };

  // We only parallelize if keep_rule needs to be applied to all classes.
  auto wq = workqueue_foreach<KeepSpec*>(
      [&process_single_keep, &classes](KeepSpec* keep_rule) {
        RegexMap regex_map;
        ClassMatcher class_match(*keep_rule);

        for (const auto& cls : classes) {
          process_single_keep(class_match, *keep_rule, cls, regex_map);
        }
      });

  for (auto& keep_rule : keep_rules) {
    RegexMap regex_map;
    ClassMatcher class_match(keep_rule);

    // This case is very fast. Just process it immediately in the main thread.
    const auto& className = keep_rule.class_spec.className;
    if (!classname_contains_wildcard(className)) {
      DexClass* cls = find_single_class(pg_map, className);
      process_single_keep(class_match, keep_rule, cls, regex_map);
      continue;
    }

    // This is also very fast. Process it in the main thread, too.
    const auto& extendsClassName = keep_rule.class_spec.extendsClassName;
    if (extendsClassName != "" &&
        !classname_contains_wildcard(extendsClassName)) {
      DexClass* super = find_single_class(pg_map, extendsClassName);
      if (super != nullptr) {
        TypeSet children;
        get_all_children(hierarchy, super->get_type(), children);
        process_single_keep(class_match, keep_rule, super, regex_map);
        for (auto const* type : children) {
          process_single_keep(
              class_match, keep_rule, type_class(type), regex_map);
        }
      }
      continue;
    }

    // Otherwise, it might take a longer time. Add to the work queue.
    wq.add_item(&keep_rule);
  }

  wq.run_all();
}

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
                            const Scope& classes,
                            const Scope& external_classes,
                            ProguardConfiguration* pg_config) {
  // Filter out duplicate rules to speed up processing.
  filter_duplicate_rules(&pg_config->keep_rules);
  filter_duplicate_rules(&pg_config->assumenosideeffects_rules);
  // Now process each of the different kinds of rules as well
  // as -assumenosideeffects and -whyareyoukeeping.

  ClassHierarchy hierarchy;
  build_extends_or_implements_hierarchy(classes, &hierarchy);
  // We need to include external classes in the hierarchy because keep rules
  // may, for instance, forbid renaming of all classes that inherit from a
  // given external class.
  build_extends_or_implements_hierarchy(external_classes, &hierarchy);

  process_keep(pg_map,
               pg_config->whyareyoukeeping_rules,
               classes,
               external_classes,
               hierarchy,
               process_whyareyoukeeping,
               "whyareyoukeeping");

  process_keep(pg_map,
               pg_config->keep_rules,
               classes,
               external_classes,
               hierarchy,
               mark_class_and_members_for_keep,
               "classes and members");

  process_keep(pg_map,
               pg_config->assumenosideeffects_rules,
               classes,
               external_classes,
               hierarchy,
               process_assumenosideeffects,
               "assumenosideeffects");

  // By default, keep all annotation classes.
  for (auto cls : classes) {
    if (is_annotation(cls)) {
      cls->rstate.set_keep();
      if (cls->rstate.report_whyareyoukeeping()) {
        TRACE(PGR,
              2,
              "whyareyoukeeping Class %s kept because it is an annotation "
              "class\n",
              redex::dexdump_name_to_dot_name(cls->get_deobfuscated_name())
                  .c_str());
      }
      cls->rstate.increment_keep_count();
    }
  }
}

} // namespace redex
