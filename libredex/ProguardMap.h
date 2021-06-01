/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "DexClass.h"
#include "ProguardLineRange.h"

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
   * Construct an empty ProGuard map.
   */
  explicit ProguardMap() = default;

  /**
   * Construct map from the given file.
   */
  explicit ProguardMap(const std::string& filename,
                       bool use_new_rename_map = false);

  /**
   * Construct map from a given stream.
   */
  explicit ProguardMap(std::istream& is) { parse_proguard_map(is); }

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

  struct Frame {
    DexString* method;
    uint32_t line;
    Frame(DexString* s, uint32_t line) : method(s), line(line) {}
  };

  /**
   * Translate obfuscated stack frame to un-obfuscated series of frames. The
   * frames should be ordered with callees preceding their callers.
   */
  std::vector<Frame> deobfuscate_frame(DexString*, uint32_t line) const;

  /**
   * Obtain line range vector for a given obfuscated method name.
   */
  ProguardLineRangeVector& method_lines(const std::string& obfuscated_method);

  bool empty() const {
    return m_classMap.empty() && m_fieldMap.empty() && m_methodMap.empty();
  }

  bool is_special_interface(const std::string& type) const {
    return m_pg_coalesced_interfaces.find(type) !=
           m_pg_coalesced_interfaces.end();
  }

 private:
  void parse_proguard_map(std::istream& fp);
  void parse_full_map(std::istream& fp);

  bool parse_class(const std::string& line);
  bool parse_field(const std::string& line);
  bool parse_method(const std::string& line);

  bool parse_class_full_format(const std::string& line);
  bool parse_field_full_format(const std::string& line);
  bool parse_method_full_format(const std::string& line);

 private:
  // Unobfuscated to obfuscated maps
  std::unordered_map<std::string, std::string> m_classMap;
  std::unordered_map<std::string, std::string> m_fieldMap;
  std::unordered_map<std::string, std::string> m_methodMap;

  // Obfuscated to unobfuscated maps from proguard
  std::unordered_map<std::string, std::string> m_obfClassMap;
  std::unordered_map<std::string, std::string> m_obfFieldMap;
  std::unordered_map<std::string, std::string> m_obfMethodMap;

  // Field map for reflection analysis when type is unknown
  // Stores Lcom/facebook/Class;.field -> original name without class name
  std::unordered_map<std::string, std::string> m_obfUntypedFieldMap;

  // Method map for reflection analysis when return type is unknown
  // Stores Lcom/facebook/Class;.method(II) -> original name without class name
  std::unordered_map<std::string, std::string> m_obfUntypedMethodMap;

  std::unordered_map<std::string, ProguardLineRangeVector> m_obfMethodLinesMap;

  // Interfaces that are (most likely) coalesced by Proguard.
  std::unordered_set<std::string> m_pg_coalesced_interfaces;

  std::string m_currClass;
  std::string m_currNewClass;
};

/**
 * Deobfuscate all items in all dexes, and cache those names in the objects
 * themselves, so that they're automatically carried through optimization
 * passes.
 */
void apply_deobfuscated_names(const std::vector<DexClasses>&,
                              const ProguardMap&);

// Exposed for testing purposes.
namespace pg_impl {

DexString* file_name_from_method_string(const DexString* method);

void apply_deobfuscated_positions(IRCode*, const ProguardMap&);

std::string lines_key(const std::string& method_name);

} // namespace pg_impl

/**
 * Convert a dot-style name to a dexdump-style name, e.g.:
 *   com.foo.MyClass -> Lcom/foo/MyClass;
 *   void -> V
 *   java.util.ArrayList[][] -> [[Ljava/util/ArrayList;
 */
std::string convert_type(std::string);
