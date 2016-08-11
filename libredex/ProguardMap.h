/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstddef>
#include <fstream>
#include <map>
#include <string>

#include "DexClass.h"

/**
 * ProguardMap parses ProGuard's mapping.txt file that maps de-obfuscated class
 * and member names to obfuscated names.  This facility is useful if you have
 * profile data that is not obfuscated and you are trying to optimize an
 * obfuscated APK.
 *
 * The proguard map format looks like this:
 *   com.foo.bar -> A:\n"
 *       int do1 -> a\n"
 *       3:3:void <init>() -> <init>\n"
 *       8:929:java.util.ArrayList getCopy() -> a\n"
 *
 * In keeping with this format, the `translate` functions in ProguardMap take a
 * de-obfuscated name and produce an obfuscated name.  Since we're likely
 * working on an obfuscated APK, this direction is also good for looking up the
 * result with the various `DexMember::get_member` functions.
 *
 * The deobfuscate* methods expect as input the "complete" name for the object.
 * For classes, this is the full descriptor.
 * For methods, it's <class descriptor>.<name>(<args descs>)<return desc> .
 * For fields,  it's <class descriptor>.<name>:<type desc> .
 */
struct ProguardMap {
  /**
   * Construct map from the given file.
   */
  explicit ProguardMap(const std::string& filename);

  /**
   * Construct map from a given stream.
   */
  explicit ProguardMap(std::istream& is) {
    parse_proguard_map(is);
  }

  /**
   * Translate un-obfuscated class name to obfuscated name.
   */
  std::string translate_class(const std::string& cls) const;

  /**
   * Translate un-obfuscated field name to obfuscated name.
   */
  std::string translate_field(const std::string& field) const;

  /**
   * Translate un-obfuscated method name to obfuscated name.
   */
  std::string translate_method(const std::string& method) const;

  /**
   * Translate obfuscated class name to un-obfuscated name.
   */
  std::string deobfuscate_class(const std::string& cls) const;

  /**
   * Translate obfuscated field name to un-obfuscated name.
   */
  std::string deobfuscate_field(const std::string& field) const;

  /**
   * Translate obfuscated method name to un-obfuscated name.
   */
  std::string deobfuscate_method(const std::string& method) const;

  /**
   * Update the class mapping. Used when we do something to the class name
   * inside of redex.
   */
  void update_class_mapping(const std::string& oldname, const std::string& newname);

  /**
   * Translate obfuscated class name to un-obfuscated name using dynamic map
   */
  std::string deobfuscate_class_dynamic(const std::string& cls) const;

  /**
   * Translate obfuscated method name to un-obfuscated name using dynamic map
   */
  std::string deobfuscate_method_dynamic(const std::string& method) const;

  /**
   * Translate obfuscated field name to un-obfuscated name using dynamic map
   */
  std::string deobfuscate_field_dynamic(const std::string& field) const;

 private:
  void parse_proguard_map(std::istream& fp);

  bool parse_class(const std::string& line);
  bool parse_field(const std::string& line);
  bool parse_method(const std::string& line);

  void add_method_mapping(
    const char* type,
    const char* methodname,
    const char* newname,
    const char* args);

 private:
  // Unobfuscated to obfuscated maps
  std::map<std::string, std::string> m_classMap;
  std::map<std::string, std::string> m_fieldMap;
  std::map<std::string, std::string> m_methodMap;

  // Obfuscated to unobfuscated maps from proguard
  std::map<std::string, std::string> m_obfClassMap;
  std::map<std::string, std::string> m_obfFieldMap;
  std::map<std::string, std::string> m_obfMethodMap;

  // Obfuscated to unobfuscated maps
  // which can be updated by redex passes
  std::map<std::string, std::string> m_dynObfClassMap;
  std::map<std::string, std::string> m_dynObfFieldMap;
  std::map<std::string, std::string> m_dynObfMethodMap;

  std::string m_currClass;
  std::string m_currNewClass;
};

std::string proguard_name(DexType* cls);
std::string proguard_name(DexClass* cls);
std::string proguard_name(DexMethod* method);
std::string proguard_name(DexField* field);
