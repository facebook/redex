/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>

#include "DexClass.h"

namespace native {

class CompilationUnit;

class Function {
 public:
  Function(const CompilationUnit* cu,
           std::string name,
           DexMethod* java_declaration)
      : m_compilation_unit(cu),
        m_name(std::move(name)),
        m_java_declarations({}) {
    if (java_declaration) {
      m_java_declarations.insert(java_declaration);
    }
  }

  const CompilationUnit* get_compilation_unit() const {
    return m_compilation_unit;
  }
  std::string get_name() const { return m_name; }
  const std::unordered_set<DexMethod*>& get_java_declarations() const {
    return m_java_declarations;
  }

  void add_java_declaration(DexMethod* method) {
    m_java_declarations.insert(method);
  }

 private:
  const CompilationUnit* m_compilation_unit;
  std::string m_name;
  std::unordered_set<DexMethod*> m_java_declarations;
};

class CompilationUnit {
 public:
  CompilationUnit(std::string name, boost::filesystem::path infodir)
      : m_name(std::move(name)),
        m_infodir_path(std::move(infodir)),
        m_name_to_functions({}) {}

  std::string get_name() const { return m_name; }
  boost::filesystem::path get_infodir_path() const { return m_infodir_path; }

  void populate_functions(const std::unordered_map<std::string, DexMethod*>&
                              expected_names_to_decl);
  std::unordered_map<std::string, Function>& get_functions() {
    return m_name_to_functions;
  }
  const std::unordered_map<std::string, Function>& get_functions() const {
    return m_name_to_functions;
  }

  Function* get_function(const std::string& name) {
    auto it = m_name_to_functions.find(name);
    if (it != m_name_to_functions.end()) {
      return &it->second;
    }
    return nullptr;
  }

 private:
  std::string m_name;
  boost::filesystem::path m_infodir_path;
  std::unordered_map<std::string, Function> m_name_to_functions;
};

std::unordered_map<std::string, CompilationUnit> get_compilation_units(
    const boost::filesystem::path& path);

struct NativeContext {
  static NativeContext build(const std::string& path_to_native_results,
                             const Scope& java_scope);
  std::unordered_map<std::string, CompilationUnit> name_to_compilation_units;
  std::unordered_map<DexMethod*, Function*> java_declaration_to_function;
};

}; // namespace native
