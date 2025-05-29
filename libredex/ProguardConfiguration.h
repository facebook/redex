/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <boost/functional/hash.hpp>
#include <functional>
#include <iosfwd>
#include <set>
#include <string>
#include <vector>

#include "DeterministicContainers.h"
#include "DexAccess.h"

namespace keep_rules {

// To hold the value in -assumenosideeffects with value.
struct AssumeReturnValue {
  AssumeReturnValue() : value_type(ValueNone) {}
  enum ValueType {
    ValueBool,
    ValueNone,
  } value_type;
  union Value {
    int v;
  } value;
};

std::ostream& operator<<(std::ostream& oss, const AssumeReturnValue& asr);

struct MemberSpecification {
  DexAccessFlags requiredSetAccessFlags = DexAccessFlags(0);
  DexAccessFlags requiredUnsetAccessFlags = DexAccessFlags(0);
  std::string annotationType;
  std::string name;
  std::string descriptor;
  AssumeReturnValue return_value;

  friend bool operator==(const MemberSpecification& lhs,
                         const MemberSpecification& rhs);
};

size_t hash_value(const MemberSpecification&);

struct ClassSpecification {
  DexAccessFlags setAccessFlags = DexAccessFlags(0);
  DexAccessFlags unsetAccessFlags = DexAccessFlags(0);
  std::string annotationType;
  struct ClassNameSpec {
    std::string name;
    bool negated{false};
    ClassNameSpec(std::string name, bool neg)
        : name(std::move(name)), negated(neg) {}

    friend bool operator==(const ClassNameSpec& lhs, const ClassNameSpec& rhs) {
      return lhs.negated == rhs.negated && lhs.name == rhs.name;
    }
  };
  std::vector<ClassNameSpec> classNames;
  std::string extendsAnnotationType; // An optional annotation for the
                                     // extends/implements type.
  std::string extendsClassName; // An optional class specification which this
                                // class extends or implements.
  std::vector<MemberSpecification> fieldSpecifications;
  std::vector<MemberSpecification> methodSpecifications;

  friend bool operator==(const ClassSpecification& lhs,
                         const ClassSpecification& rhs);

  std::string class_names_str() const {
    std::string s;
    for (const auto& className : classNames) {
      if (className.negated) {
        s += "!";
      }
      s += className.name;
    }
    return s;
  }
};

size_t hash_value(const ClassSpecification&);

struct KeepSpec {
  // "includedescriptorclasses" is not implemented. We just parse this option
  // and save for the future, but the actual behavior is not implemented.
  bool includedescriptorclasses{false};
  bool allowshrinking{false};
  bool allowoptimization{false}; // Same. Not implemented.
  bool allowobfuscation{false};
  bool mark_classes{true};
  bool mark_conditionally{false};
  ClassSpecification class_spec;
  // For debugging and analysis
  std::string source_filename;
  uint32_t source_line;

  KeepSpec() = default;
  // Each keep rule in the PG file will correspond to exactly one unique
  // instance of a KeepSpec in Redex. This makes it efficient and simple to
  // represent these KeepSpecs in the reachability graph.
  KeepSpec(const KeepSpec&) = delete;

  friend bool operator==(const KeepSpec& lhs, const KeepSpec& rhs);
  friend std::ostream& operator<<(std::ostream&, const KeepSpec&);
};

size_t hash_value(const KeepSpec&);

inline size_t hash_value(const std::unique_ptr<KeepSpec>& spec) {
  return hash_value(*spec);
}

/*
 * This is a simple implementation of a set that preserves insertion order. The
 * insertion order of keep rules reflects their order in the input .pro files.
 * files. At present, the effects of keep rule application on the
 * ReferencedState are order-sensitive, hence the need for this.
 *
 * XXX: We may have bugs with parallelization due to this order-sensitivity...
 * we should probably fix / spec out more precisely the subset of features of
 * the PG keep rules that we wish to support.
 */
class KeepSpecSet {
 public:
  using iterator = std::vector<KeepSpec*>::const_iterator;
  using const_iterator = std::vector<KeepSpec*>::const_iterator;

  void emplace(std::unique_ptr<KeepSpec> spec) {
    const auto& p = m_unordered_set.emplace(std::move(spec));
    bool did_insert = p.second;
    if (did_insert) {
      m_ordered.push_back(p.first->get());
    }
  }

  iterator begin() const { return m_ordered.begin(); }

  iterator end() const { return m_ordered.end(); }

  const std::vector<KeepSpec*>& elements() const { return m_ordered; }

  size_t size() const { return m_ordered.size(); }

  bool empty() const { return begin() == end(); }

  void erase_if(const std::function<bool(const KeepSpec&)>&);

  template <typename Pred>
  iterator stable_partition(Pred p);

 private:
  std::vector<KeepSpec*> m_ordered;
  UnorderedSet<std::unique_ptr<KeepSpec>,
               boost::hash<std::unique_ptr<KeepSpec>>>
      m_unordered_set;
};

template <typename Pred>
inline KeepSpecSet::iterator KeepSpecSet::stable_partition(Pred p) {
  return std::stable_partition(m_ordered.begin(), m_ordered.end(), p);
}

struct ProguardConfiguration {
  bool ok;
  std::vector<std::string> includes;
  std::set<std::string> already_included;
  std::string basedirectory;
  std::vector<std::string> injars;
  std::vector<std::string> outjars;
  std::vector<std::string> libraryjars;
  std::vector<std::string> printmapping;
  std::vector<std::string> printconfiguration;
  std::vector<std::string> printseeds;
  std::vector<std::string> printusage;
  std::vector<std::string> keepdirectories;
  bool shrink{true};
  bool optimize{true};
  bool allowaccessmodification{false};
  bool dontobfuscate{false};
  bool dontusemixedcaseclassnames{false};
  bool dontpreverify{false};
  bool verbose{false};
  std::string target_version;
  KeepSpecSet keep_rules;
  std::optional<KeepSpecSet::iterator> keep_rules_native_begin;
  KeepSpecSet assumenosideeffects_rules;
  KeepSpecSet assumevalues_rules;
  KeepSpecSet whyareyoukeeping_rules;
  std::vector<std::string> optimization_filters;
  std::vector<std::string> keepattributes;
  std::vector<std::string> dontwarn;
  std::vector<std::string> keeppackagenames;

  // If set to true `-basedirectory` will be ignored.
  bool frozen_basedirectory{false};

  ProguardConfiguration() = default;
  ProguardConfiguration(const ProguardConfiguration&) = delete;
};

namespace impl {

/*
 * This class exposes private methods of ReferencedState and is only intended
 * to be used by ProguardMatcher and related PG-config-handling logic.
 * Optimizations should use functions defined in ReachableClasses.h instead.
 */
class KeepState {
 public:
  template <class DexMember>
  static bool has_keep(DexMember* member) {
    return member->rstate.has_keep();
  }

  template <class DexMember, class... Args>
  static void set_has_keep(DexMember* member, Args&&... args) {
    member->rstate.set_has_keep(std::forward<Args>(args)...);
  }

  template <class DexMember>
  static bool allowshrinking(DexMember* member) {
    return member->rstate.allowshrinking();
  }

  template <class DexMember>
  static void set_allowshrinking(DexMember* member) {
    member->rstate.set_allowshrinking();
  }

  template <class DexMember>
  static void unset_allowshrinking(DexMember* member) {
    member->rstate.unset_allowshrinking();
  }

  template <class DexMember>
  static bool allowobfuscation(DexMember* member) {
    return member->rstate.allowobfuscation();
  }

  template <class DexMember>
  static void set_allowobfuscation(DexMember* member) {
    member->rstate.set_allowobfuscation();
  }

  template <class DexMember>
  static void unset_allowobfuscation(DexMember* member) {
    member->rstate.unset_allowobfuscation();
  }

  template <class DexMember>
  static bool includedescriptorclasses(DexMember* member) {
    return member->rstate.includedescriptorclasses();
  }

  template <class DexMember>
  static void set_includedescriptorclasses(DexMember* member) {
    member->rstate.set_includedescriptorclasses();
  }
};

} // namespace impl

} // namespace keep_rules
