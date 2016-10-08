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
#include "Timer.h"

namespace {

std::string find_or_same(
  const std::string& key,
  const std::unordered_map<std::string, std::string>& map
) {
  auto it = map.find(key);
  if (it == map.end()) return key;
  return it->second;
}

std::string convert_scalar_type(std::string type) {
  static const std::unordered_map<std::string, std::string> prim_map =
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

std::string convert_field(std::string cls, std::string type, std::string name) {
  return cls + "." + name + ":" + type;
}

std::string convert_method(
  std::string cls,
  std::string rtype,
  std::string methodname,
  std::string args
) {
  return cls + "." + methodname + ":(" + args + ")" + rtype;
}

std::string translate_type(std::string type, const ProguardMap& pm) {
  auto base_start = type.find_first_not_of("[");
  auto array_prefix = type.substr(0, base_start);
  auto base_type = type.substr(base_start);
  return array_prefix + pm.translate_class(base_type);
}

void whitespace(const char*& p) {
  while (isspace(*p)) {
    ++p;
  }
}

void digits(const char*& p) {
  while (isdigit(*p)) {
    ++p;
  }
}

bool isseparator(uint32_t cp) {
  return cp == '\0' ||
    cp == ' ' ||
    cp == ':' ||
    cp == ',' ||
    cp == '\n' ||
    cp == '(' ||
    cp == ')';
}

bool id(const char*& p, std::string& s) {
  auto b = p;
  auto first = mutf8_next_code_point(p);
  if (isdigit(first)) return false;
  while (true) {
    auto prev = p;
    auto cp = mutf8_next_code_point(p);
    if (isseparator(cp)) {
      p = prev;
      s = std::string(b, p - b);
      return true;
    }
  }
}

bool literal(const char*& p, const char* s) {
  auto len = strlen(s);
  bool rv = !strncmp(p, s, len);
  p += len;
  return rv;
}

bool literal(const char*& p, char s) {
  if (*p == s) {
    ++p;
    return true;
  }
  return false;
}
}

ProguardMap::ProguardMap(const std::string& filename) {
  if (!filename.empty()) {
    Timer t("Parsing proguard map");
    std::ifstream fp(filename);
    always_assert_log(fp, "Can't open proguard map: %s\n", filename.c_str());
    parse_proguard_map(fp);
  }
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

void ProguardMap::parse_proguard_map(std::istream& fp) {
  std::string line;
  while (std::getline(fp, line)) {
    parse_class(line);
  }
  fp.clear();
  fp.seekg(0);
  assert_log(!fp.fail(), "Can't use ProguardMap with non-seekable stream");
  while (std::getline(fp, line)) {
    if (parse_class(line)) {
      continue;
    }
    if (parse_field(line)) {
      continue;
    }
    if (parse_method(line)) {
      continue;
    }
    always_assert_log(false,
                      "Bogus line encountered in proguard map: %s\n",
                      line.c_str());
  }
}

bool ProguardMap::parse_class(const std::string& line) {
  std::string classname;
  std::string newname;
  auto p = line.c_str();
  if (!id(p, classname)) return false;
  if (!literal(p, " -> ")) return false;
  if (!id(p, newname)) return false;
  m_currClass = convert_type(classname);
  m_currNewClass = convert_type(newname);
  m_classMap[m_currClass] = m_currNewClass;
  m_obfClassMap[m_currNewClass] = m_currClass;
  return true;
}

bool ProguardMap::parse_field(const std::string& line) {
  std::string type;
  std::string fieldname;
  std::string newname;

  auto p = line.c_str();
  whitespace(p);
  if (!id(p, type)) return false;
  whitespace(p);
  if (!id(p, fieldname)) return false;
  if (!literal(p, " -> ")) return false;
  if (!id(p, newname)) return false;

  auto ctype = convert_type(type);
  auto xtype = translate_type(ctype, *this);
  auto pgnew = convert_field(m_currNewClass, xtype.c_str(), newname);
  auto pgold = convert_field(m_currClass, ctype.c_str(), fieldname);
  m_fieldMap[pgold] = pgnew;
  m_obfFieldMap[pgnew] = pgold;
  return true;
}

bool ProguardMap::parse_method(const std::string& line) {
  std::string type;
  std::string methodname;
  std::string old_args;
  std::string new_args;
  std::string newname;

  auto p = line.c_str();
  whitespace(p);
  digits(p);
  literal(p, ':');
  digits(p);
  literal(p, ':');

  if (!id(p, type)) return false;
  whitespace(p);

  if (!id(p, methodname)) return false;

  if (!literal(p, '(')) return false;
  while (true) {
    std::string arg;
    if (literal(p, ')')) break;
    id(p, arg);
    auto old_arg = convert_type(arg);
    auto new_arg = translate_type(old_arg, *this);
    old_args += old_arg;
    new_args += new_arg;
    literal(p, ',');
  }

  literal(p, ':');
  digits(p);
  literal(p, ':');
  digits(p);
  literal(p, " -> ");

  if (!id(p, newname)) return false;

  auto old_rtype = convert_type(type);
  auto new_rtype = translate_type(old_rtype, *this);
  auto pgold = convert_method(m_currClass, old_rtype, methodname, old_args);
  auto pgnew = convert_method(m_currNewClass, new_rtype, newname, new_args);
  m_methodMap[pgold] = pgnew;
  m_obfMethodMap[pgnew] = pgold;
  return true;
}

void apply_deobfuscated_names(
  const std::vector<DexClasses>& dexen,
  const ProguardMap& pm
) {
  for (auto const& dex : dexen) {
    for (auto const& cls : dex) {
      TRACE(PGR, 4, "deob cls %s %s\n",
            proguard_name(cls).c_str(),
            pm.deobfuscate_class(proguard_name(cls)).c_str());
      cls->set_deobfuscated_name(pm.deobfuscate_class(proguard_name(cls)));
      for (auto const& m : cls->get_dmethods()) {
        TRACE(PGR, 4, "deob dmeth %s %s\n",
              proguard_name(m).c_str(),
              pm.deobfuscate_method(proguard_name(m)).c_str());
        m->set_deobfuscated_name(pm.deobfuscate_method(proguard_name(m)));
      }
      for (auto const& m : cls->get_vmethods()) {
        TRACE(PM, 4, "deob vmeth %s %s\n",
              proguard_name(m).c_str(),
              pm.deobfuscate_method(proguard_name(m)).c_str());
        m->set_deobfuscated_name(pm.deobfuscate_method(proguard_name(m)));
      }

      for (auto const& f : cls->get_ifields()) {
        TRACE(PM, 4, "deob ifield %s %s\n",
              proguard_name(f).c_str(),
              pm.deobfuscate_field(proguard_name(f)).c_str());
        f->set_deobfuscated_name(pm.deobfuscate_field(proguard_name(f)));
      }
      for (auto const& f : cls->get_sfields()) {
        TRACE(PM, 4, "deob sfield %s %s\n",
              proguard_name(f).c_str(),
              pm.deobfuscate_field(proguard_name(f)).c_str());
        f->set_deobfuscated_name(pm.deobfuscate_field(proguard_name(f)));
      }
    }
  }
}

std::string proguard_name(const DexType* type) {
  return type->get_name()->c_str();
}

std::string proguard_name(const DexClass* cls) {
  return cls->get_name()->c_str();
}

std::string proguard_name(const DexMethod* method) {
  // Format is <class descriptor>.<method name>:(<arg descriptors>)<return descriptor>
  auto str = proguard_name(method->get_class()) + "." + method->get_name()->c_str() + ":";

  auto proto = method->get_proto();
  auto args_str = std::string("(");

  for (auto& arg_type: proto->get_args()->get_type_list()) {
    args_str += proguard_name(arg_type);
  }
  args_str += ")";

  auto ret_str = proguard_name(proto->get_rtype());
  return str + args_str + ret_str;
}

std::string proguard_name(const DexField* field) {
  auto str = proguard_name(field->get_class()) + "."
    + field->get_name()->c_str() + ":"
    + proguard_name(field->get_type());

  return str;
}

std::string convert_type(std::string type) {
  auto dimpos = type.find('[');
  if (dimpos == std::string::npos) {
    return convert_scalar_type(type);
  }
  auto ndims = std::count(type.begin() + dimpos, type.end(), '[');
  std::string res(ndims, '[');
  return res + convert_scalar_type(type.substr(0, dimpos));
}
