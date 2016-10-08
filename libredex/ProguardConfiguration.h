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

namespace redex {

  enum class AccessFlag {
    PUBLIC,
    PRIVATE,
    PROTECTED,
    STATIC,
    FINAL,
    INTERFACE,
    SYNCHRONIZED,
    VOLATILE,
    TRANSIENT,
    BRIDGE,
    VARARGS,
    NATIVE,
    ABSTRACT,
    STRICT,
    SYNTHETIC,
    ANNOTATION,
    ENUM
  };

  struct MemberSpecification {
    std::set<AccessFlag> requiredSetAccessFlags;
    std::set<AccessFlag> requiredUnsetAccessFlags;
    std::string annotationType;
    std::string name;
    std::string descriptor;
  };

  struct ClassSpecification {
    std::set<AccessFlag> setAccessFlags;
    std::set<AccessFlag>  unsetAccessFlags;
    std::string annotationType;
    std::string className;
    std::string extendsAnnotationType; // An optional annotation for the extends/implements type.
    std::string extendsClassName; // An optional class specification which this class extends or implements.
    std::vector<MemberSpecification> fieldSpecifications;
    std::vector<MemberSpecification> methodSpecifications;
  };

  struct KeepSpec {
    bool includedescriptorclasses = false;
    bool allowshrinking = false;
    bool allowoptimization = false;
    bool allowobfuscation = false;
    ClassSpecification class_spec;
  };

  struct ProguardConfiguration {
    bool ok;
    std::vector<std::string> includes;
    std::string basedirectory;
    std::vector<std::string> injars;
    std::vector<std::string> outjars;
    std::vector<std::string> libraryjars;
    std::vector<std::string> printmapping;
    std::vector<std::string> printconfiguration;
    std::vector<std::string> printseeds;
    std::vector<std::string> printusage;
    std::vector<std::string> keepdirectories;
    bool shrink = true;
    bool optimize = true;
    bool allowaccessmodification = false;
    bool dontobfuscate = false;
    bool dontusemixedcaseclassnames = false;
    bool dontpreverify = false;
    bool verbose = false;
    std::string target_version;
    std::vector<KeepSpec> keep_rules;
    std::vector<KeepSpec> keepclassmembers_rules;
    std::vector<KeepSpec> keepclasseswithmembers_rules;
    std::vector<KeepSpec> assumesideeffects_rules;
    std::vector<std::string> optimization_filters;
    std::vector<std::string> keepattributes;
    std::vector<std::string> dontwarn;
    std::vector<std::string> keeppackagenames;
  };

} // namespace redex
