/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <sstream>

#include "Debug.h"
#include "VerifyUtil.h"

DexClass* find_class_named(const DexClasses& classes, const char* name) {
  auto it =
      std::find_if(classes.begin(), classes.end(), [&name](DexClass* cls) {
        return !strcmp(name, cls->get_name()->c_str());
      });
  return it == classes.end() ? nullptr : *it;
}

DexField* find_ifield_named(const DexClass& cls, const char* name) {
  auto fields = cls.get_ifields();
  auto it =
      std::find_if(fields.begin(), fields.end(), [&name](const DexField* f) {
        return strcmp(name, f->get_name()->c_str()) == 0;
      });
  return it == fields.end() ? nullptr : *it;
}

DexField* find_sfield_named(const DexClass& cls, const char* name) {
  auto fields = cls.get_sfields();
  auto it =
      std::find_if(fields.begin(), fields.end(), [&name](const DexField* f) {
        return strcmp(name, f->get_name()->c_str()) == 0;
      });
  return it == fields.end() ? nullptr : *it;
}

DexField* find_field_named(const DexClass& cls, const char* name) {
  auto ret = find_ifield_named(cls, name);
  if (ret) {
    return ret;
  }
  return find_sfield_named(cls, name);
}

DexMethod* find_vmethod_named(const DexClass& cls, const char* name) {
  auto vmethods = cls.get_vmethods();
  auto it =
      std::find_if(vmethods.begin(), vmethods.end(), [&name](DexMethod* m) {
        return strcmp(name, m->get_name()->c_str()) == 0;
      });
  return it == vmethods.end() ? nullptr : *it;
}

DexMethod* find_dmethod_named(const DexClass& cls, const char* name) {
  auto dmethods = cls.get_dmethods();
  auto it =
      std::find_if(dmethods.begin(), dmethods.end(), [&name](DexMethod* m) {
        return strcmp(name, m->get_name()->c_str()) == 0;
      });
  return it == dmethods.end() ? nullptr : *it;
}

DexMethod* find_method_named(const DexClass& cls, const char* name) {
  auto ret = find_dmethod_named(cls, name);
  if (ret != nullptr) {
    return ret;
  }
  return find_vmethod_named(cls, name);
}

DexOpcodeMethod* find_invoke(const DexMethod* m,
                             DexOpcode opcode,
                             const char* target_mname,
                             DexType* receiver) {
  auto insns = m->get_dex_code()->get_instructions();
  return find_invoke(
      insns.begin(), insns.end(), opcode, target_mname, receiver);
}

DexOpcodeMethod* find_invoke(std::vector<DexInstruction*>::iterator begin,
                             std::vector<DexInstruction*>::iterator end,
                             DexOpcode opcode,
                             const char* target_mname,
                             DexType* receiver) {
  auto it = std::find_if(
      begin, end, [opcode, target_mname, receiver](DexInstruction* insn) {
        if (insn->opcode() != opcode) {
          return false;
        }
        auto meth = static_cast<DexOpcodeMethod*>(insn)->get_method();
        if (receiver && meth->get_class() != receiver) {
          return false;
        }
        auto mname =
            static_cast<DexOpcodeMethod*>(insn)->get_method()->get_name();
        return mname == DexString::get_string(target_mname);
      });
  return it == end ? nullptr : static_cast<DexOpcodeMethod*>(*it);
}

// Given a semicolon delimited list of extracted files from the APK, return a
// map of the original APK's file path to its path on disk.
ResourceFiles decode_resource_paths(const char* location, const char* suffix) {
  ResourceFiles files;
  std::istringstream input;
  input.str(location);
  for (std::string file_path; std::getline(input, file_path, ':');) {
    auto pos = file_path.rfind('/');
    always_assert(pos >= 0 && pos + 1 < file_path.length());
    auto directory = file_path.substr(0, pos);
    if (boost::algorithm::ends_with(directory, suffix)) {
      auto original_name = file_path.substr(pos + 1);
      // Undo simple escaping at buck_imports/redex_utils
      boost::replace_all(original_name, "zC", ":");
      boost::replace_all(original_name, "zS", "/");
      boost::replace_all(original_name, "zZ", "z");
      files.emplace(original_name, file_path);
    }
  }
  return files;
}

DexInstruction* find_instruction(DexMethod* m, DexOpcode opcode) {
  auto& insns = m->get_dex_code()->get_instructions();
  auto it =
      std::find_if(insns.begin(), insns.end(), [opcode](DexInstruction* insn) {
        return insn->opcode() == opcode;
      });
  return it == insns.end() ? nullptr : *it;
}

void verify_type_erased(const DexClass* cls, size_t num_dmethods) {
  ASSERT_NE(cls, nullptr);
  auto dmethods = cls->get_dmethods();
  ASSERT_EQ(dmethods.size(), num_dmethods);
  for (auto m : dmethods) {
    ASSERT_FALSE(method::is_init(m));
    ASSERT_NE(m->c_str(), "<init>");
  }
  const auto& vmethods = cls->get_vmethods();
  ASSERT_TRUE(vmethods.empty());
}
