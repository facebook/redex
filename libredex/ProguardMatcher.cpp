/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <boost/regex.hpp>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "ClassHierarchy.h"
#include "ConcurrentContainers.h"
#include "DexAnnotation.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "ProguardMatcher.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardRegex.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "Show.h"
#include "StringBuilder.h"
#include "Timer.h"
#include "Trace.h"
#include "WorkQueue.h"

using namespace keep_rules;

namespace {

using RegexMap = std::unordered_map<std::string, boost::regex>;

std::unique_ptr<boost::regex> make_rx(const std::string& s,
                                      bool convert = true) {
  if (s.empty()) return nullptr;
  auto wc = convert ? proguard_parser::convert_wildcard_type(s) : s;
  auto rx = proguard_parser::form_type_regex(wc);
  return std::make_unique<boost::regex>(rx);
}

std::vector<std::unique_ptr<boost::regex>> make_rxs(
    const std::vector<ClassSpecification::ClassNameSpec>& strs) {
  std::vector<std::unique_ptr<boost::regex>> rxs;
  rxs.reserve(strs.size());
  for (const auto& str : strs) {
    rxs.push_back(make_rx(str.name));
  }
  return rxs;
}

std::string_view get_deobfuscated_name(const DexType* type) {
  auto cls = type_class(type);
  if (cls == nullptr) {
    return type->str();
  }
  return cls->get_deobfuscated_name().str();
}

const char* get_deobfuscated_name_cstr(const DexType* type) {
  auto cls = type_class(type);
  if (cls == nullptr) {
    return type->c_str();
  }
  return cls->get_deobfuscated_name().c_str();
}

bool match_annotation_rx(const DexClass* cls, const boost::regex& annorx) {
  const auto* annos = cls->get_anno_set();
  if (!annos) return false;
  for (const auto& anno : annos->get_annotations()) {
    if (boost::regex_match(get_deobfuscated_name_cstr(anno->type()), annorx)) {
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
        m_class_names(ks.class_spec.classNames),
        m_cls(make_rxs(ks.class_spec.classNames)),
        m_anno(make_rx(ks.class_spec.annotationType, false)),
        m_extends(make_rx(ks.class_spec.extendsClassName)),
        m_extends_anno(make_rx(ks.class_spec.extendsAnnotationType, false)) {}

  bool match(const DexClass* cls) {
    for (std::size_t i = 0; i < m_class_names.size(); i++) {
      const auto& class_name = m_class_names[i];

      // Check for class name match `match_name` is really slow; let's
      // short-circuit it for wildcard-only matches.
      if (class_name.name != "*" && class_name.name != "**" &&
          !match_name(cls, i)) {
        continue;
      }

      if (class_name.negated) {
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
      if (!match_extends(cls)) {
        return false;
      }
      return true;
    }
    return false;
  }

 private:
  bool match_name(const DexClass* cls, int index) const {
    auto deob_name_cstr = cls->get_deobfuscated_name().c_str();
    const auto& rx = *m_cls[index];
    return boost::regex_match(deob_name_cstr, rx);
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
    if (cls->get_type() == type::java_lang_Object()) return false;
    // First check to see if an annotation type needs to be matched.
    if (m_extends_anno) {
      if (!match_annotation_rx(cls, *m_extends_anno)) {
        return false;
      }
    }
    auto deob_name_cstr = cls->get_deobfuscated_name().c_str();
    return boost::regex_match(deob_name_cstr, *m_extends);
  }

  bool search_interfaces(const DexClass* cls) {
    const auto* interfaces = cls->get_interfaces();
    if (!interfaces) return false;
    for (const auto& impl : *interfaces) {
      auto impl_class = type_class(impl);
      if (impl_class) {
        if (type_and_annotation_match(impl_class) ||
            search_extends_and_interfaces(impl_class)) {
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
    // Do any of the classes and interfaces above match?
    auto super_type = cls->get_super_class();
    if (super_type && super_type != type::java_lang_Object()) {
      auto super_class = type_class(super_type);
      if (super_class) {
        if (type_and_annotation_match(super_class) ||
            search_extends_and_interfaces(super_class)) {
          return true;
        }
      }
    }
    // Do any of the interfaces from here and up match?
    return search_interfaces(cls);
  }

  DexAccessFlags setFlags_;
  DexAccessFlags unsetFlags_;
  std::vector<ClassSpecification::ClassNameSpec> m_class_names;
  std::vector<std::unique_ptr<boost::regex>> m_cls;
  std::unique_ptr<boost::regex> m_anno;
  std::unique_ptr<boost::regex> m_extends;
  std::unique_ptr<boost::regex> m_extends_anno;

  std::unordered_map<const DexClass*, bool> m_extends_result_cache;
};

enum class RuleType {
  WHY_ARE_YOU_KEEPING,
  KEEP,
  ASSUME_NO_SIDE_EFFECTS,
  KEEP_NATIVE,
};

bool rule_type_is_keep(RuleType rule_type) {
  switch (rule_type) {
  case RuleType::KEEP:
  case RuleType::KEEP_NATIVE:
    return true;
  case RuleType::ASSUME_NO_SIDE_EFFECTS:
  case RuleType::WHY_ARE_YOU_KEEPING:
    return false;
  }
}

std::string to_string(RuleType rule_type) {
  switch (rule_type) {
  case RuleType::WHY_ARE_YOU_KEEPING:
    return "whyareyoukeeping";
  case RuleType::KEEP:
    return "classes and members";
  case RuleType::ASSUME_NO_SIDE_EFFECTS:
    return "assumenosideeffects";
  case RuleType::KEEP_NATIVE:
    return "classes with native members";
  }
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
    for (const auto& impl : *cls->get_interfaces()) {
      (*hierarchy)[impl].insert(type);
    }
  }
}

/*
 * This class contains the logic for matching against a single keep rule.
 */
class KeepRuleMatcher {
 public:
  KeepRuleMatcher(RuleType rule_type,
                  const KeepSpec& keep_rule,
                  RegexMap& regex_map)
      : m_rule_type(rule_type),
        m_keep_rule(keep_rule),
        m_regex_map(regex_map) {}

  ~KeepRuleMatcher() {
    TRACE(PGR, 3, "%s matched %zu classes and %zu members",
          show_keep(m_keep_rule).c_str(), m_class_matches, m_member_matches);
  }

  void keep_processor(DexClass*);

  void mark_class_and_members_for_keep(DexClass* cls);

  bool any_method_matches(const DexClass* cls,
                          const MemberSpecification& method_keep,
                          const boost::regex& method_regex);

  // Check that each method keep matches at least one method in :cls.
  bool all_method_keeps_match(
      const std::vector<MemberSpecification>& method_keeps,
      const DexClass* cls);

  bool any_field_matches(const DexClass* cls,
                         const MemberSpecification& field_keep);

  // Check that each field keep matches at least one field in :cls.
  bool all_field_keeps_match(
      const std::vector<MemberSpecification>& field_keeps, const DexClass* cls);

  void process_whyareyoukeeping(DexClass* cls);

  bool process_mark_conditionally(const DexClass* cls);

  void process_assumenosideeffects(DexClass* cls);

  template <class DexMember>
  void apply_rule(DexMember*);

  void apply_field_keeps(const DexClass* cls);

  void apply_method_keeps(const DexClass* cls);

  template <class Container>
  void keep_fields(const Container& fields,
                   const MemberSpecification& fieldSpecification,
                   const boost::regex& fieldname_regex);

  template <class Container>
  void keep_methods(const MemberSpecification& methodSpecification,
                    const Container& methods,
                    const boost::regex& method_regex);

  bool field_level_match(const MemberSpecification& fieldSpecification,
                         const DexField* field,
                         const boost::regex& fieldname_regex);

  bool method_level_match(const MemberSpecification& methodSpecification,
                          const DexMethod* method,
                          const boost::regex& method_regex);

  template <class DexMember>
  bool has_annotation(const DexMember* member,
                      const std::string& annotation) const;

  const boost::regex& register_matcher(const std::string& regex) const {
    auto it = m_regex_map.find(regex);
    if (it == m_regex_map.end()) {
      it = m_regex_map.emplace(regex, boost::regex{regex}).first;
    }
    return it->second;
  }

  bool is_unused() const {
    return m_class_matches == 0 && m_member_matches == 0;
  }

 private:
  void maybe_warn(const std::string& warning) {
    std::unique_lock<std::mutex> lock{m_warn_mutex};
    if (m_already_warned.count(warning) > 0) {
      return;
    }
    m_already_warned.emplace(warning);
    std::cerr << warning << std::endl;
  }

  size_t m_member_matches{0};
  size_t m_class_matches{0};
  RuleType m_rule_type;
  const KeepSpec& m_keep_rule;
  RegexMap& m_regex_map;

  std::mutex m_warn_mutex;
  std::unordered_set<std::string> m_already_warned;
};

class ProguardMatcher {
 public:
  ProguardMatcher(const ProguardMap& pg_map,
                  const Scope& classes,
                  const Scope& external_classes)
      : m_pg_map(pg_map),
        m_classes(classes),
        m_external_classes(external_classes) {
    build_extends_or_implements_hierarchy(m_classes, &m_hierarchy);
    // We need to include external classes in the hierarchy because keep rules
    // may, for instance, forbid renaming of all classes that inherit from a
    // given external class.
    build_extends_or_implements_hierarchy(m_external_classes, &m_hierarchy);
  }

  void process_proguard_rules(const ProguardConfiguration& pg_config);
  void mark_all_annotation_classes_as_keep();

  void process_keep(KeepSpecSet::iterator keep_rules_begin,
                    KeepSpecSet::iterator keep_rules_end,
                    RuleType rule_type,
                    bool process_external = false);

  DexClass* find_single_class(const std::string& descriptor) const;

  const ConcurrentSet<const KeepSpec*>& get_unused_rules() const {
    return m_unused_rules;
  }

 private:
  const ProguardMap& m_pg_map;
  const Scope& m_classes;
  const Scope& m_external_classes;
  ClassHierarchy m_hierarchy;
  ConcurrentSet<const KeepSpec*> m_unused_rules;
};

template <class DexMember>
void apply_assume_field_return_value(const KeepSpec& k, DexMember* member) {
  for (auto& field_spec : k.class_spec.fieldSpecifications) {
    auto field_val = field_spec.return_value;
    switch (field_val.value_type) {
    case keep_rules::AssumeReturnValue::ValueBool:
      always_assert(type::is_boolean(member->get_type()));
      g_redex->set_field_value(member, field_val);
      continue;
    case keep_rules::AssumeReturnValue::ValueNone:
      g_redex->unset_field_value(member);
      continue;
    }
  }
}

template <class DexMember>
void apply_assume_method_return_value(const KeepSpec& k, DexMember* member) {
  for (auto& method_spec : k.class_spec.methodSpecifications) {
    auto return_val = method_spec.return_value;
    switch (return_val.value_type) {
    case keep_rules::AssumeReturnValue::ValueBool:
      always_assert(type::is_boolean(member->get_proto()->get_rtype()));
      g_redex->set_return_value(member, return_val);
      continue;
    case keep_rules::AssumeReturnValue::ValueNone:
      g_redex->unset_return_value(member);
      continue;
    }
  }
}

// Updates a class, field or method to add keep modifiers.
// Note: allowoptimization is not implemented.
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
    if (!impl::KeepState::has_keep(member)) {
      impl::KeepState::set_allowshrinking(member);
    } else {
      // We already observed a keep rule for this member. So, even if another
      // "-keep,allowshrinking" tries to set allowshrinking, we must ignore it.
    }
  } else {
    // Otherwise reset it: don't allow shrinking.
    impl::KeepState::unset_allowshrinking(member);
  }
  // The same case: unsetting allowobfuscation has a priority.
  if (k.allowobfuscation) {
    if (!impl::KeepState::has_keep(member) &&
        strcmp(member->get_name()->c_str(), "<init>") != 0) {
      impl::KeepState::set_allowobfuscation(member);
    }
  } else {
    impl::KeepState::unset_allowobfuscation(member);
  }

  // Always apply `includedescriptorclasses` if it was set by a keep rule.
  if (k.includedescriptorclasses) {
    impl::KeepState::set_includedescriptorclasses(member);
  }
}

template <class DexMember>
bool KeepRuleMatcher::has_annotation(const DexMember* member,
                                     const std::string& annotation) const {
  auto annos = member->get_anno_set();
  if (annos == nullptr) {
    return false;
  }
  if (!proguard_parser::has_special_char(annotation)) {
    for (const auto& anno : annos->get_annotations()) {
      if (get_deobfuscated_name(anno->type()) == annotation) {
        return true;
      }
    }
  } else {
    auto annotation_regex = proguard_parser::form_type_regex(annotation);
    const boost::regex& annotation_matcher = register_matcher(annotation_regex);
    for (const auto& anno : annos->get_annotations()) {
      if (boost::regex_match(get_deobfuscated_name_cstr(anno->type()),
                             annotation_matcher)) {
        return true;
      }
    }
  }
  return false;
}

// From a fully qualified descriptor for a field, extract just the
// name of the field which occurs between the ;. and : characters.
const char* extract_field_name_cstr(const std::string& qualified_fieldname) {
  auto p = qualified_fieldname.find(";.");
  if (p == std::string::npos) {
    return qualified_fieldname.c_str();
  }
  return qualified_fieldname.c_str() + p + 2;
}

const char* extract_method_name_and_type_cstr(
    const DexString& qualified_fieldname) {
  auto p = qualified_fieldname.str().find(";.");
  return qualified_fieldname.c_str() + p + 2;
}

bool KeepRuleMatcher::field_level_match(
    const MemberSpecification& fieldSpecification,
    const DexField* field,
    const boost::regex& fieldname_regex) {
  // Check for annotation guards.
  if (!(fieldSpecification.annotationType.empty())) {
    if (!has_annotation(field, fieldSpecification.annotationType)) {
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
  auto dequalified_name_cstr =
      extract_field_name_cstr(field->get_deobfuscated_name());
  return boost::regex_match(dequalified_name_cstr, fieldname_regex);
}

template <class Container>
void KeepRuleMatcher::keep_fields(const Container& fields,
                                  const MemberSpecification& fieldSpecification,
                                  const boost::regex& fieldname_regex) {
  for (DexField* field : fields) {
    if (!field_level_match(fieldSpecification, field, fieldname_regex)) {
      continue;
    }
    if (rule_type_is_keep(m_rule_type)) {
      apply_keep_modifiers(m_keep_rule, field);
    }
    if (m_rule_type == RuleType::ASSUME_NO_SIDE_EFFECTS) {
      apply_assume_field_return_value(m_keep_rule, field);
    }
    apply_rule(field);
  }
}

std::string field_regex(const MemberSpecification& field_spec) {
  string_builders::StaticStringBuilder<3> ss;
  ss << proguard_parser::form_member_regex(field_spec.name);
  ss << "\\:";
  ss << proguard_parser::form_type_regex(field_spec.descriptor);
  return ss.str();
}

void KeepRuleMatcher::apply_field_keeps(const DexClass* cls) {
  for (const auto& field_spec : m_keep_rule.class_spec.fieldSpecifications) {
    auto fieldname_regex = field_regex(field_spec);
    const boost::regex& matcher = register_matcher(fieldname_regex);
    keep_fields(cls->get_ifields(), field_spec, matcher);
    keep_fields(cls->get_sfields(), field_spec, matcher);
  }
}

bool KeepRuleMatcher::method_level_match(
    const MemberSpecification& methodSpecification,
    const DexMethod* method,
    const boost::regex& method_regex) {
  // Check to see if the method match is guarded by an annotation match.
  if (!(methodSpecification.annotationType.empty())) {
    if (!has_annotation(method, methodSpecification.annotationType)) {
      return false;
    }
  }
  if (!access_matches(methodSpecification.requiredSetAccessFlags,
                      methodSpecification.requiredUnsetAccessFlags,
                      method->get_access())) {
    return false;
  }
  auto dequalified_name_cstr =
      extract_method_name_and_type_cstr(method->get_deobfuscated_name());
  return boost::regex_match(dequalified_name_cstr, method_regex);
}

template <class Container>
void KeepRuleMatcher::keep_methods(
    const MemberSpecification& methodSpecification,
    const Container& methods,
    const boost::regex& method_regex) {
  for (DexMethod* method : methods) {
    if (method_level_match(methodSpecification, method, method_regex)) {
      bool could_delete = can_delete(method);
      switch (m_rule_type) {
      case RuleType::KEEP:
      case RuleType::KEEP_NATIVE: {
        apply_keep_modifiers(m_keep_rule, method);
        break;
      }
      case RuleType::ASSUME_NO_SIDE_EFFECTS:
        apply_assume_method_return_value(m_keep_rule, method);
        break;
      case RuleType::WHY_ARE_YOU_KEEPING:
        break;
      }
      apply_rule(method);

      if (m_rule_type == RuleType::KEEP_NATIVE) {
        if (could_delete && !can_delete(method)) {
          g_redex->blanket_native_root_methods.insert(method);
        }
      }
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

void KeepRuleMatcher::apply_method_keeps(const DexClass* cls) {
  auto methodSpecifications = m_keep_rule.class_spec.methodSpecifications;
  for (auto& method_spec : methodSpecifications) {
    auto qualified_method_regex = method_regex(method_spec);
    const boost::regex& method_regex = register_matcher(qualified_method_regex);
    keep_methods(method_spec, cls->get_vmethods(), method_regex);
    keep_methods(method_spec, cls->get_dmethods(), method_regex);
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

bool KeepRuleMatcher::any_method_matches(const DexClass* cls,
                                         const MemberSpecification& method_keep,
                                         const boost::regex& method_regex) {
  auto match = [&](const DexMethod* method) {
    return method_level_match(method_keep, method, method_regex);
  };
  return std::any_of(cls->get_vmethods().begin(), cls->get_vmethods().end(),
                     match) ||
         std::any_of(cls->get_dmethods().begin(), cls->get_dmethods().end(),
                     match);
}

// Check that each method keep matches at least one method in :cls.
bool KeepRuleMatcher::all_method_keeps_match(
    const std::vector<MemberSpecification>& method_keeps, const DexClass* cls) {
  return std::all_of(method_keeps.begin(),
                     method_keeps.end(),
                     [&](const MemberSpecification& method_keep) {
                       auto qualified_method_regex = method_regex(method_keep);
                       const boost::regex& matcher =
                           register_matcher(qualified_method_regex);
                       return any_method_matches(cls, method_keep, matcher);
                     });
}

bool KeepRuleMatcher::any_field_matches(const DexClass* cls,
                                        const MemberSpecification& field_keep) {
  auto fieldtype_regex = field_regex(field_keep);
  const boost::regex& matcher = register_matcher(fieldtype_regex);
  auto match = [&](const DexField* field) {
    return field_level_match(field_keep, field, matcher);
  };
  return std::any_of(cls->get_ifields().begin(), cls->get_ifields().end(),
                     match) ||
         std::any_of(cls->get_sfields().begin(), cls->get_sfields().end(),
                     match);
}

// Check that each field keep matches at least one field in :cls.
bool KeepRuleMatcher::all_field_keeps_match(
    const std::vector<MemberSpecification>& field_keeps, const DexClass* cls) {
  return std::all_of(field_keeps.begin(),
                     field_keeps.end(),
                     [&](const MemberSpecification& field_keep) {
                       return any_field_matches(cls, field_keep);
                     });
}

bool KeepRuleMatcher::process_mark_conditionally(const DexClass* cls) {
  const auto& class_spec = m_keep_rule.class_spec;
  if (class_spec.fieldSpecifications.empty() &&
      class_spec.methodSpecifications.empty()) {
    std::cerr << "WARNING: A keepclasseswithmembers rule for class "
              << class_spec.class_names_str()
              << " has no field or member specifications.\n";
  }
  return all_field_keeps_match(class_spec.fieldSpecifications, cls) &&
         all_method_keeps_match(class_spec.methodSpecifications, cls);
}

// Once a match has been made against a class i.e. the class name
// matches, the annotations match, the extends clause matches and the
// access modifier filters match, then start to apply the keep control
// bits to the class, members and appropriate classes and members
// in the class hierarchy.
//
// Parallelization note: We parallelize process_keep, and this function will be
// eventually executed concurrently. There are potential races in rstate:
// (1) m_keep and (2) m_(un)set_allow(shrinking|obfuscation). These values are
// always overwritten. These WAW (write-after-write) races are benign and do not
// affect the results.
void KeepRuleMatcher::mark_class_and_members_for_keep(DexClass* cls) {
  // First check to see if we need to mark conditionally to see if all
  // field and method rules match i.e. we have a -keepclasseswithmembers
  // rule to process.
  if (m_keep_rule.mark_conditionally) {
    // If this class does not incur at least one match for each field
    // and method rule, then don't mark this class or its members.
    if (!process_mark_conditionally(cls)) {
      return;
    }
  }
  if (m_keep_rule.allowoptimization) {
    std::ostringstream oss;
    oss << "WARNING: 'allowoptimization' keep modifier is NOT implemented: "
        << show_keep(m_keep_rule);
    maybe_warn(oss.str());
  }
  if (m_keep_rule.mark_classes || m_keep_rule.mark_conditionally) {
    bool could_delete = can_delete(cls);
    apply_keep_modifiers(m_keep_rule, cls);
    impl::KeepState::set_has_keep(cls, &m_keep_rule);
    if (m_rule_type == RuleType::KEEP_NATIVE) {
      if (could_delete && !can_delete(cls)) {
        g_redex->blanket_native_root_classes.insert(cls);
      }
    }
    ++m_class_matches;
    if (cls->rstate.report_whyareyoukeeping()) {
      TRACE(PGR, 2, "whyareyoukeeping Class %s kept by %s",
            java_names::internal_to_external(cls->get_deobfuscated_name().str())
                .c_str(),
            show_keep(m_keep_rule).c_str());
    }
  }
  // Walk up the hierarchy performing seed marking.
  DexClass* class_to_mark = cls;
  while (class_to_mark != nullptr && !class_to_mark->is_external()) {
    // Mark unconditionally.
    apply_field_keeps(class_to_mark);
    apply_method_keeps(class_to_mark);
    auto typ = class_to_mark->get_super_class();
    if (typ == nullptr) {
      break;
    }
    class_to_mark = type_class(typ);
  }
}

// This function is also executed concurrently.
void KeepRuleMatcher::process_whyareyoukeeping(DexClass* cls) {
  cls->rstate.set_whyareyoukeeping();

  apply_field_keeps(cls);
  // Set any method-level keep whyareyoukeeping bits.
  apply_method_keeps(cls);
}

// This function is also executed concurrently.
void KeepRuleMatcher::process_assumenosideeffects(DexClass* cls) {
  cls->rstate.set_assumenosideeffects();

  // Apply any method-level keep specifications.
  apply_method_keeps(cls);
}

template <class DexMember>
void KeepRuleMatcher::apply_rule(DexMember* member) {
  switch (m_rule_type) {
  case RuleType::WHY_ARE_YOU_KEEPING:
    member->rstate.set_whyareyoukeeping();
    break;
  case RuleType::KEEP:
  case RuleType::KEEP_NATIVE: {
    impl::KeepState::set_has_keep(member, &m_keep_rule);
    ++m_member_matches;
    if (member->rstate.report_whyareyoukeeping()) {
      TRACE(PGR, 2, "whyareyoukeeping %s kept by %s", SHOW(member),
            show_keep(m_keep_rule).c_str());
    }
    if (impl::KeepState::includedescriptorclasses(member)) {
      std::vector<DexType*> types;
      member->gather_types_shallow(types);
      for (auto* type : types) {
        if (auto* cls = type_class(type)) {
          impl::KeepState::set_has_keep(cls, &m_keep_rule);
          if (cls->rstate.report_whyareyoukeeping()) {
            TRACE(PGR, 2, "whyareyoukeeping %s kept by %s", SHOW(cls),
                  show_keep(m_keep_rule).c_str());
          }
        }
      }
    }
    break;
  }
  case RuleType::ASSUME_NO_SIDE_EFFECTS:
    member->rstate.set_assumenosideeffects();
    break;
  }
}

void KeepRuleMatcher::keep_processor(DexClass* cls) {
  switch (m_rule_type) {
  case RuleType::WHY_ARE_YOU_KEEPING:
    process_whyareyoukeeping(cls);
    break;
  case RuleType::KEEP:
  case RuleType::KEEP_NATIVE:
    mark_class_and_members_for_keep(cls);
    break;
  case RuleType::ASSUME_NO_SIDE_EFFECTS:
    process_assumenosideeffects(cls);
    break;
  }
}

DexClass* ProguardMatcher::find_single_class(
    const std::string& descriptor) const {
  auto const& dsc = java_names::external_to_internal(descriptor);
  DexType* typ = DexType::get_type(m_pg_map.translate_class(dsc));
  if (typ == nullptr) {
    typ = DexType::get_type(dsc);
    if (typ == nullptr) {
      return nullptr;
    }
  }
  return type_class(typ);
}

void ProguardMatcher::process_keep(KeepSpecSet::iterator keep_rules_begin,
                                   KeepSpecSet::iterator keep_rules_end,
                                   RuleType rule_type,
                                   bool process_external) {
  Timer t("Process keep for " + to_string(rule_type));

  // Classes are aligned by 8. Shard size should be (co-)prime for good
  // distribution.
  constexpr size_t LOCKS = 1039u;
  std::array<std::mutex, LOCKS> locks;
  auto get_lock = [&locks](const DexClass* cls) -> std::mutex& {
    return locks[reinterpret_cast<uintptr_t>(cls) % LOCKS];
  };

  auto process_single_keep = [process_external,
                              &get_lock](ClassMatcher& class_match,
                                         KeepRuleMatcher& rule_matcher,
                                         DexClass* cls) {
    // Skip external classes.
    if (cls == nullptr || (!process_external && cls->is_external())) {
      return;
    }
    if (class_match.match(cls)) {
      std::unique_lock<std::mutex> lock(get_lock(cls));
      rule_matcher.keep_processor(cls);
    }
  };

  // We only parallelize if keep_rule needs to be applied to all classes.
  auto wq = workqueue_foreach<const KeepSpec*>([&](const KeepSpec* keep_rule) {
    RegexMap regex_map;
    ClassMatcher class_match(*keep_rule);
    KeepRuleMatcher rule_matcher(rule_type, *keep_rule, regex_map);

    for (const auto& cls : m_classes) {
      process_single_keep(class_match, rule_matcher, cls);
    }
    if (process_external) {
      for (const auto& cls : m_external_classes) {
        process_single_keep(class_match, rule_matcher, cls);
      }
    }

    if (rule_matcher.is_unused()) {
      m_unused_rules.insert(keep_rule);
    }
  });

  RegexMap regex_map;
  for (auto it = keep_rules_begin; it != keep_rules_end; ++it) {
    const auto& keep_rule = *(*it);
    ClassMatcher class_match(keep_rule);

    bool has_negation = std::any_of(keep_rule.class_spec.classNames.begin(),
                                    keep_rule.class_spec.classNames.end(),
                                    [](const auto& v) { return v.negated; });

    if (!has_negation) {
      // This case is very fast. Just process it immediately in the main thread.
      bool class_with_wildcard = false;
      for (const auto& className : keep_rule.class_spec.classNames) {
        if (!classname_contains_wildcard(className.name)) {
          DexClass* cls = find_single_class(className.name);
          KeepRuleMatcher rule_matcher(rule_type, keep_rule, regex_map);
          process_single_keep(class_match, rule_matcher, cls);
          if (rule_matcher.is_unused()) {
            m_unused_rules.insert(&keep_rule);
          }
        } else {
          class_with_wildcard = true;
        }
      }
      if (!class_with_wildcard) {
        continue;
      }

      // This is also very fast. Process it in the main thread, too.
      const auto& extendsClassName = keep_rule.class_spec.extendsClassName;
      if (!extendsClassName.empty() &&
          !classname_contains_wildcard(extendsClassName)) {
        DexClass* super = find_single_class(extendsClassName);
        if (super != nullptr) {
          KeepRuleMatcher rule_matcher(rule_type, keep_rule, regex_map);
          auto children = get_all_children(m_hierarchy, super->get_type());
          process_single_keep(class_match, rule_matcher, super);
          for (auto const* type : children) {
            process_single_keep(class_match, rule_matcher, type_class(type));
          }
          if (rule_matcher.is_unused()) {
            m_unused_rules.insert(&keep_rule);
          }
        }
        continue;
      }
    }

    TRACE(PGR, 2, "Slow rule: %s", show_keep(keep_rule).c_str());
    // Otherwise, it might take a longer time. Add to the work queue.
    wq.add_item(&keep_rule);
  }

  wq.run_all();
}

void ProguardMatcher::process_proguard_rules(
    const ProguardConfiguration& pg_config) {
  // Now process each of the different kinds of rules as well
  // as -assumenosideeffects and -whyareyoukeeping.
  process_keep(pg_config.whyareyoukeeping_rules.begin(),
               pg_config.whyareyoukeeping_rules.end(),
               RuleType::WHY_ARE_YOU_KEEPING);

  auto keep_rules_native_begin =
      pg_config.keep_rules_native_begin.value_or(pg_config.keep_rules.end());

  process_keep(pg_config.keep_rules.begin(), keep_rules_native_begin,
               RuleType::KEEP);

  process_keep(keep_rules_native_begin,
               pg_config.keep_rules.end(),
               RuleType::KEEP_NATIVE);

  process_keep(pg_config.assumenosideeffects_rules.begin(),
               pg_config.assumenosideeffects_rules.end(),
               RuleType::ASSUME_NO_SIDE_EFFECTS,
               /* process_external = */ true);
}

void ProguardMatcher::mark_all_annotation_classes_as_keep() {
  for (auto cls : m_classes) {
    if (is_annotation(cls)) {
      impl::KeepState::set_has_keep(cls, keep_reason::ANNO);
      if (cls->rstate.report_whyareyoukeeping()) {
        TRACE(PGR,
              2,
              "whyareyoukeeping Class %s kept because it is an annotation "
              "class\n",
              java_names::internal_to_external(
                  cls->get_deobfuscated_name_or_empty())
                  .c_str());
      }
    }
  }
}

} // namespace

namespace keep_rules {

ConcurrentSet<const KeepSpec*> process_proguard_rules(
    const ProguardMap& pg_map,
    const Scope& classes,
    const Scope& external_classes,
    const ProguardConfiguration& pg_config,
    bool keep_all_annotation_classes) {
  ProguardMatcher pg_matcher(pg_map, classes, external_classes);
  pg_matcher.process_proguard_rules(pg_config);
  if (keep_all_annotation_classes) {
    pg_matcher.mark_all_annotation_classes_as_keep();
  }
  return pg_matcher.get_unused_rules();
}

namespace testing {

bool matches(const KeepSpec& ks, const DexClass* c) {
  ClassMatcher cm{ks};
  return cm.match(c);
}

} // namespace testing

} // namespace keep_rules
