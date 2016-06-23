/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ProguardMap.h"

#include <algorithm>
#include <cstring>

#include "Debug.h"

namespace {

constexpr size_t kBufSize = 4096;

std::string convert_scalar_type(const char* type) {
  static const std::map<std::string, std::string> prim_map =
    {{"void",    "V"},
     {"boolean", "Z"},
     {"byte",    "B"},
     {"short",   "S"},
     {"char",    "C"},
     {"int",     "I"},
     {"long",    "J"},
     {"float",   "F"},
     {"double",  "D"}};
  auto it = prim_map.find(type);
  if (it != prim_map.end()) {
    return it->second;
  }
  std::string ret(type);
  std::replace(ret.begin(), ret.end(), '.', '/');
  return std::string("L") + ret + ";";
}

std::string convert_type(const char* type) {
  if (type[strlen(type) - 1] == ']') {
    std::string elem_type(type, strrchr(type, '[') - type);
    return std::string("[") + convert_type(elem_type.c_str());
  }
  return convert_scalar_type(type);
}

std::string convert_args(const char* args) {
  const char* p = args;
  std::string ret;
  while (*p != '\0') {
    const char* n = strchr(p, ',');
    if (n) {
      std::string arg(p, n - p);
      ret += convert_type(arg.c_str());
      p = n + 1;
    } else {
      ret += convert_type(p);
      break;
    }
  }
  return ret;
}

std::string convert_proguard_field(
  const std::string& cls,
  const char* type,
  const char* name) {
  return cls + "." + name + ":" + convert_type(type);
}

std::string convert_proguard_method(
  const std::string& cls,
  const char* rtype,
  const char* methodname,
  const char* args) {
  return cls + "." +
    std::string(methodname) +
    "(" + convert_args(args) + ")" +
    convert_type(rtype);
}

}

ProguardMap::ProguardMap(const std::string& filename) {
  if (!filename.empty()) {
    std::ifstream fp(filename);
    always_assert_log(fp, "Can't open proguard map: %s\n", filename.c_str());
    parse_proguard_map(fp);
  }
}

std::string find_or_same(
  const std::string& key,
  const std::map<std::string, std::string>& map
) {
  auto it = map.find(key);
  if (it == map.end()) return key;
  return it->second;
}

std::string ProguardMap::translate_class(const std::string& cls) const {
  return find_or_same(cls, m_classMap);
}

std::string ProguardMap::translate_field(const std::string& field) const {
  return find_or_same(field, m_fieldMap);
}

std::string ProguardMap::translate_method(const std::string& method) const {
  return find_or_same(method, m_methodMap);
}

std::string ProguardMap::deobfuscate_class(const std::string& cls) const {
  return find_or_same(cls, m_obfClassMap);
}

std::string ProguardMap::deobfuscate_field(const std::string& field) const {
  return find_or_same(field, m_obfFieldMap);
}

std::string ProguardMap::deobfuscate_method(const std::string& method) const {
  return find_or_same(method, m_obfMethodMap);
}

std::string ProguardMap::deobfuscate_class_dynamic(const std::string& cls) const {
  return find_or_same(cls, m_dynObfClassMap);
}

std::string ProguardMap::deobfuscate_method_dynamic(const std::string& method) const {
  return find_or_same(method, m_dynObfMethodMap);
}

std::string ProguardMap::deobfuscate_field_dynamic(const std::string& field) const {
  return find_or_same(field, m_dynObfFieldMap);
}

void ProguardMap::parse_line(const std::string& line) {
  if (parse_class(line)) {
    return;
  }
  if (parse_field(line)) {
    return;
  }
  if (parse_method(line)) {
    return;
  }
  always_assert_log(false,
                    "Bogus line encountered in proguard map: %s\n",
                    line.c_str());
}

bool ProguardMap::parse_class(const std::string& line) {
  char classname[kBufSize];
  char newname[kBufSize];
  int n;
  n = sscanf(line.c_str(), "%s -> %[^:]:", classname, newname);
  if (n != 2) {
    return false;
  }
  m_currClass = convert_type(classname);
  m_currNewClass = convert_type(newname);
  m_classMap[m_currClass] = m_currNewClass;
  m_obfClassMap[m_currNewClass] = m_currClass;
  // initialize the dynamic map with the PG mapping
  m_dynObfClassMap[m_currNewClass] = m_currClass;
  return true;
}

bool ProguardMap::parse_field(const std::string& line) {
  char type[kBufSize];
  char fieldname[kBufSize];
  char newname[kBufSize];
  int n = sscanf(line.c_str(), " %s %[a-zA-Z0-9$_] -> %s",
                 type, fieldname, newname);
  if (n != 3) {
    return false;
  }
  auto pgnew = convert_proguard_field(m_currNewClass, type, newname);
  auto pgold = convert_proguard_field(m_currClass, type, fieldname);
  m_fieldMap[pgold] = pgnew;
  m_obfFieldMap[pgnew] = pgold;
  // initialize the dynamic map with the PG mapping
  m_dynObfFieldMap[pgnew] = pgold;
  return true;
}

bool ProguardMap::parse_method(const std::string& line) {
  const char* type;
  const char* methodname;
  const char* args;
  const char* newname;

  char* linecopy = strdup(line.c_str());
  char* p = linecopy;

  while (!isalpha(*p)) {
    if (!*p) goto no_match;
    p++;
  }
  type = p;

  while (!isspace(*p)) {
    if (!*p) goto no_match;
    p++;
  }
  *p++ = '\0';
  methodname = p;

  while (*p != '(') {
    if (!*p) goto no_match;
    p++;
  }
  *p++ = '\0';
  args = p;

  while (*p != ')') {
    if (!*p) goto no_match;
    p++;
  }
  *p++ = '\0';

  while (*p != ' ') {
    if (!*p) goto no_match;
    p++;
  }

  if (strncmp(p, " -> ", 4)) {
    goto no_match;
  }
  p += 4;
  newname = p;

  add_method_mapping(type, methodname, newname, args);
  free(linecopy);
  return true;

 no_match:
  free(linecopy);
  return false;
}

void ProguardMap::add_method_mapping(
  const char* type,
  const char* methodname,
  const char* newname,
  const char* args) {
  auto pgold = convert_proguard_method(m_currClass, type, methodname, args);
  auto pgnew = convert_proguard_method(m_currNewClass, type, newname, args);
  m_methodMap[pgold] = pgnew;
  m_obfMethodMap[pgnew] = pgold;
  // initialize the dynamic map with the PG mapping
  m_dynObfMethodMap[pgnew] = pgold;
}

void ProguardMap::update_class_mapping(const std::string& oldname, const std::string& newname) {
  auto unobf_clsname = find_or_same(oldname, m_dynObfClassMap);
  m_dynObfClassMap[newname] = unobf_clsname;
}

void ProguardMap::update_method_mapping(const DexMethod* dm, const std::string& new_str) {
  std::string cls(dm->get_class()->get_name()->c_str());
  auto type = dm->get_proto()->get_rtype()->get_name()->c_str();
  auto method = dm->get_name()->c_str();
  auto args = SHOW(dm->get_proto()->get_args());
  auto oldname = convert_proguard_method(cls, type, method, args);
  auto newname = convert_proguard_method(cls, type, new_str.c_str(), args);
  auto unobf_methodname = find_or_same(oldname, m_dynObfMethodMap);
  m_dynObfClassMap[newname] = unobf_methodname;
}

void ProguardMap::update_field_mapping(const DexField* df, const std::string& new_str) {
  std::string cls(df->get_class()->get_name()->c_str());
  auto type = df->get_type()->get_name()->c_str();
  auto field = df->get_name()->c_str();
  auto oldname = convert_proguard_field(cls, type, field);
  auto newname = convert_proguard_field(cls, type, new_str.c_str());
  auto unobf_fieldname = find_or_same(oldname, m_dynObfFieldMap);
  m_dynObfClassMap[newname] = unobf_fieldname;
}

std::string proguard_name(DexType* type) {
  return type->get_name()->c_str();
}

std::string proguard_name(DexClass* cls) {
  return cls->get_name()->c_str();
}

std::string proguard_name(DexMethod* method) {
  // Format is <class descriptor>.<method name>(<arg descriptors>)<return descriptor>
  auto str = proguard_name(method->get_class()) + "." + method->get_name()->c_str();

  auto proto = method->get_proto();
  auto args_str = std::string("(");

  for (auto& arg_type: proto->get_args()->get_type_list()) {
    args_str += proguard_name(arg_type);
  }
  args_str += ")";

  auto ret_str = proguard_name(proto->get_rtype());
  return str + args_str + ret_str;
}

std::string proguard_name(DexField* field) {
  auto str = proguard_name(field->get_class()) + "."
    + field->get_name()->c_str() + ":"
    + proguard_name(field->get_type());

  return str;
}
