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

const std::string& ProguardMap::translate_class(const std::string& cls) {
  auto it = m_classMap.find(cls);
  if (it == m_classMap.end()) return cls;
  return it->second;
}

const std::string& ProguardMap::translate_field(const std::string& field) {
  return m_fieldMap[field];
}

const std::string& ProguardMap::translate_method(const std::string& method) {
  return m_methodMap[method];
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
}
