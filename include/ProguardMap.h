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
 */
struct ProguardMap {
  /**
   * Construct map from the given file.
   */
  explicit ProguardMap(const std::string& filename);

  /**
   * Construct map from a given stream.
   */
  template <class IStream>
  explicit ProguardMap(IStream& is) {
    parse_proguard_map(is);
  }

  /**
   * Translate un-obfuscated class name to obfuscated name.
   */
  std::string translate_class(const std::string& cls);

  /**
   * Translate un-obfuscated field name to obfuscated name.
   */
  std::string translate_field(const std::string& field);

  /**
   * Translate un-obfuscated method name to obfuscated name.
   */
  std::string translate_method(const std::string& method);

 private:
  template <class IStream>
  void parse_proguard_map(IStream& fp) {
    std::string line;
    while (std::getline(fp, line)) {
      parse_line(line);
    }
  }

  void parse_line(const std::string& line);
  bool parse_class(const std::string& line);
  bool parse_field(const std::string& line);
  bool parse_method(const std::string& line);

  void add_method_mapping(
      const char* type,
      const char* methodname,
      const char* newname,
      const char* args);

 private:
  std::map<std::string, std::string> m_classMap;
  std::map<std::string, std::string> m_fieldMap;
  std::map<std::string, std::string> m_methodMap;
  std::string m_currClass;
  std::string m_currNewClass;
};
