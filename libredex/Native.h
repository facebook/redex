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

class Function {
 public:
  Function(std::string name, DexMethod* java_declaration)
      : m_name(std::move(name)), m_java_declaration(java_declaration) {}
  std::string get_name() const { return m_name; }
  DexMethod* get_java_declaration() const { return m_java_declaration; }

 private:
  std::string m_name;
  /* nullable */ DexMethod* m_java_declaration;
};

class CompilationUnit {
 public:
  CompilationUnit(std::string name, boost::filesystem::path infodir)
      : m_name(std::move(name)), m_infodir_path(std::move(infodir)) {}

  std::string get_name() const { return m_name; }
  boost::filesystem::path get_infodir_path() const { return m_infodir_path; }

  void populate_functions(const std::unordered_map<std::string, DexMethod*>&
                              expected_names_to_decl);
  std::vector<Function>& get_functions() { return m_functions; }
  const std::vector<Function>& get_functions() const { return m_functions; }

 private:
  std::string m_name;
  boost::filesystem::path m_infodir_path;
  std::vector<Function> m_functions;
};

std::vector<CompilationUnit> get_compilation_units(
    const boost::filesystem::path& path);

struct NativeContext {
  static NativeContext build(const std::string& path_to_native_results,
                             const Scope& java_scope);
  std::vector<CompilationUnit> compilation_units;
  std::unordered_map<std::string, Function*> name_to_function;
  std::unordered_map<DexMethod*, Function*> java_declaration_to_function;
};

}; // namespace native
