/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <filesystem>

#include "DexClass.h"

namespace native {

class SoLibrary;

class Function {
 public:
  Function(const SoLibrary* lib, std::string name, DexMethod* java_declaration)
      : m_so_library(lib), m_name(std::move(name)), m_java_declarations({}) {
    if (java_declaration) {
      m_java_declarations.insert(java_declaration);
    }
  }

  const SoLibrary* get_so_library() const { return m_so_library; }
  std::string get_name() const { return m_name; }
  const std::unordered_set<DexMethod*>& get_java_declarations() const {
    return m_java_declarations;
  }

  void add_java_declaration(DexMethod* method) {
    m_java_declarations.insert(method);
  }

 private:
  const SoLibrary* m_so_library;
  std::string m_name;
  std::unordered_set<DexMethod*> m_java_declarations;
};

class SoLibrary {
 public:
  SoLibrary(std::string name, std::filesystem::path json)
      : m_name(std::move(name)),
        m_json_path(std::move(json)),
        m_name_to_functions({}) {}

  std::string get_name() const { return m_name; }
  std::filesystem::path get_json_path() const { return m_json_path; }

  void populate_functions();
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
  std::filesystem::path m_json_path;
  std::unordered_map<std::string, Function> m_name_to_functions;
};

std::vector<SoLibrary> get_so_libraries(const std::filesystem::path& path);

struct NativeContext {
  static NativeContext build(const std::string& path_to_native_results,
                             const Scope& java_scope);
  std::vector<SoLibrary> so_libraries;
  std::unordered_map<DexMethod*, Function*> java_declaration_to_function;
};

}; // namespace native

extern std::unique_ptr<native::NativeContext> g_native_context;

namespace native {

inline Function* get_native_function_for_dex_method(DexMethod* m) {
  auto it = g_native_context->java_declaration_to_function.find(m);
  if (it != g_native_context->java_declaration_to_function.end()) {
    return it->second;
  }
  return nullptr;
}
} // namespace native
