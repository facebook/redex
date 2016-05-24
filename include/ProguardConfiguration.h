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
#include <vector>

namespace proguard_parser {
  
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
    set<AccessFlag> requiredAccessFlags;
    set<AccessFlag> unsetAccessFlags;
    string annotationType;
    string name;
    string descriptor;
  };
  
  struct ClassSpecification {
    set<AccessFlag> setAccessFlags;
    set<AccessFlag>  unsetAccessFlags;
    string annotationType;
    string className;
    string extendsAnnotationType; // An optional annotation for the extends/implements type.
    string extendsClassName; // An optional class specification which this class extends or implements.
    vector<MemberSpecification> fieldSpecifications;
    vector<MemberSpecification> methodSpecifications;
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
    vector<string> includes;
    vector<string> injars;
    vector<string> outjars;
    vector<string> libraryjars;
    vector<string> printmapping;
    vector<string> printconfiguration;
    vector<string> printseeds;
    vector<string> printusage;
    vector<string> keepdirectories;
    bool shrink = true;
    bool allowaccessmodification = false;
    bool dontusemixedcaseclassnames = false;
    bool dontpreverify = false;
    bool verbose = false;
    string target_version;
    vector<KeepSpec> keep_rules;
    vector<KeepSpec> keepclassmembers_rules;
    vector<KeepSpec> keepclasseswithmembers_rules;
    vector<string> optimization_filters;
    vector<string> keepattributes;
    vector<string> dontwarn;
  };
  
}

