/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <set>
#include <string>
#include <vector>

#include "DexAccess.h"

namespace redex {

struct MemberSpecification {
  mutable unsigned long count{0};
  DexAccessFlags requiredSetAccessFlags = DexAccessFlags(0);
  DexAccessFlags requiredUnsetAccessFlags = DexAccessFlags(0);
  std::string annotationType;
  std::string name;
  std::string descriptor;
  bool mark_conditionally{false};
};

struct ClassSpecification {
  DexAccessFlags setAccessFlags = DexAccessFlags(0);
  DexAccessFlags unsetAccessFlags = DexAccessFlags(0);
  std::string annotationType;
  std::string className;
  std::string extendsAnnotationType; // An optional annotation for the
                                     // extends/implements type.
  std::string extendsClassName; // An optional class specification which this
                                // class extends or implements.
  std::vector<MemberSpecification> fieldSpecifications;
  std::vector<MemberSpecification> methodSpecifications;
};

struct KeepSpec {
  mutable unsigned long count{0};
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
};

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
  std::vector<KeepSpec> keep_rules;
  std::vector<KeepSpec> assumenosideeffects_rules;
  std::vector<KeepSpec> whyareyoukeeping_rules;
  std::vector<std::string> optimization_filters;
  std::vector<std::string> keepattributes;
  std::vector<std::string> dontwarn;
  std::vector<std::string> keeppackagenames;
};

} // namespace redex
