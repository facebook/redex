/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>

#pragma once

struct keeprules {

  enum ClassType {
    CLASS =       1 << 0,
    INTERFACE =   1 << 1,
    ENUMERATION = 1 << 2,
    ANNOTATION =  1 << 3,
  };


  static const int ANY_CLASS_TYPE = CLASS | INTERFACE | ENUMERATION | ANNOTATION;

  enum MemberModifier {
    PUBLIC =    1 << 0,
    PRIVATE =   1 << 1,
    PROTECTED = 1 << 2,
    STATIC =    1 << 3,
    FINAL =     1 << 4,
    TRANSIENT = 1 << 5,
    NATIVE =    1 << 6,
  };



  //int ANY_MODIFIER = PUBLIC | PRIVATE | PROTECTED | STATIC | FINAL | TRANSIENT | NATIVE;


};

struct FieldFilter {
  FieldFilter(uint32_t _flags, const char* _annotation, const char* _name, const char* _type) :
    flags(_flags),
    annotation(_annotation),
    name(_name),
    type(_type)
  {}

  uint32_t flags;
  const char* annotation;
  const char* name;
  const char* type;

  bool is_public()    { return flags & keeprules::PUBLIC; }
  bool is_private()   { return flags & keeprules::PRIVATE; }
  bool is_protected() { return flags & keeprules::PROTECTED; }
  bool is_static()    { return flags & keeprules::STATIC; }
  bool is_final()     { return flags & keeprules::FINAL; }
  bool is_transient() { return flags & keeprules::TRANSIENT; }
};

struct MethodFilter {
  MethodFilter(uint32_t _flags, const char* _name, const char* _return_type) :
    flags(_flags),
    name(_name),
    return_type(_return_type)
  {}

  uint32_t flags;
  const char* name;
  const char* return_type;
  std::vector<char*> params;

  bool is_public()    { return flags & keeprules::PUBLIC; }
  bool is_private()   { return flags & keeprules::PRIVATE; }
  bool is_protected() { return flags & keeprules::PROTECTED; }
  bool is_static()    { return flags & keeprules::STATIC; }
  bool is_final()     { return flags & keeprules::FINAL; }
  bool is_native()    { return flags & keeprules::NATIVE; }
};

struct KeepRule {
  KeepRule() {}

  std::string print_class_type(int ct) {
    std::string p;
    switch (ct) {
      case keeprules::ClassType::CLASS:
        p = "CLASS";
        break;
      case keeprules::ClassType::INTERFACE:
        p = "INTERFACE";
        break;
      case keeprules::ClassType::ENUMERATION:
        p = "ENUMERATION";
        break;
      case keeprules::ClassType::ANNOTATION:
        p = "ANNOTATION";
        break;
    }
    return p;
  }

  std::string print_flags(int f) {
    std::string p = "";
    if (f & keeprules::MemberModifier::PUBLIC) {
      p += "PUBLIC ";
    }
    if (f & keeprules::MemberModifier::PRIVATE) {
      p += "PRIVATE ";
    }
    if (f & keeprules::MemberModifier::PROTECTED) {
      p += "PROTECTED ";
    }
    if (f & keeprules::MemberModifier::STATIC) {
      p += "STATIC ";
    }
    if (f & keeprules::MemberModifier::FINAL) {
      p += "FINAL ";
    }
    if (f & keeprules::MemberModifier::TRANSIENT) {
      p += "TRANSIENT ";
    }
    if (f & keeprules::MemberModifier::NATIVE) {
      p += "NATIVE ";
    }
    return p;
  }

  std::string show() {
    std::stringstream printout;
    printout << "type:" << print_class_type(class_type);
    printout << " flags:" << print_flags(flags);
    printout << " cls/mem rename:" << allow_cls_rename << allow_member_rename;
    if (annotation) {
      printout << " anno:" << annotation;
    }
    if (classname) {
      printout << " pattern:" << classname;
    }
    if (extends) {
      printout << " extends:" << extends;
    }
    printout << " FieldFilters: ";
    for (auto & f : fields) {
      printout << f.name << " ";
    }
    printout << " MethodFilters: ";
    for (auto & m : methods) {
      printout << m.name << " ";
    }
    printout << std::endl;

    return printout.str();
  }

  int class_type;
  int32_t flags;
  const char* annotation = nullptr;
  const char* classname = nullptr;
  const char* extends = nullptr;
  bool allow_deletion;
  bool allow_cls_rename;
  bool allow_member_rename;
  std::vector<FieldFilter> fields;
  std::vector<MethodFilter> methods;
};

bool pattern_match(const char* pattern, const char* name, int pl, int nl);

bool type_matches(const char* pattern, const char* name, int pl, int nl);

bool parse_proguard_file(const char * input, std::vector<KeepRule>* rules);
