/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Native.h"

#include <fstream>
#include <json/json.h>

#include "NativeNames.h"
#include "Show.h"
#include "Walkers.h"

namespace fs = boost::filesystem;

namespace native {

void get_compilation_units_impl(std::vector<CompilationUnit>& compilation_units,
                                const std::string& location_prefix,
                                const fs::path& path) {

  if (!fs::is_directory(path)) {
    return;
  }

  if (fs::is_regular_file(path / "jni.json")) {
    // We found a compilation unit.
    compilation_units.emplace_back(location_prefix, path);
    return;
  }

  std::string separator = location_prefix.empty() ? "" : "/";

  for (const auto& item : fs::directory_iterator(path)) {
    std::string new_location_prefix =
        location_prefix + separator + item.path().filename().string();
    get_compilation_units_impl(compilation_units, new_location_prefix, item);
  }
}

std::vector<CompilationUnit> get_compilation_units(const fs::path& path) {
  std::vector<CompilationUnit> ret;
  get_compilation_units_impl(ret, "", path);
  return ret;
}

void CompilationUnit::populate_functions(
    const std::unordered_map<std::string, DexMethod*>& expected_names_to_decl) {
  auto jni_json_path = m_infodir_path / "jni.json";
  std::ifstream jni_json_stream(jni_json_path.string());
  always_assert_log(
      jni_json_stream, "Cannot find file %s", jni_json_path.string().c_str());
  Json::Value json;
  jni_json_stream >> json;
  for (Json::Value::ArrayIndex i = 0; i < json.size(); i++) {
    std::string function_name = json[i].asString();
    auto decl_it = expected_names_to_decl.find(function_name);
    DexMethod* decl =
        decl_it == expected_names_to_decl.end() ? nullptr : decl_it->second;
    m_functions.emplace_back(std::move(function_name), decl);
  }
}

NativeContext NativeContext::build(const std::string& path_to_native_results,
                                   const Scope& java_scope) {
  NativeContext ret;
  fs::path path(path_to_native_results);

  std::unordered_map<std::string, DexMethod*> expected_names_to_decl;
  walk::methods(java_scope, [&](DexMethod* m) {
    if (is_native(m)) {
      std::string short_name =
          native_names::get_native_short_name_for_method(m);
      std::string long_name = native_names::get_native_long_name_for_method(m);
      expected_names_to_decl[short_name] = m;
      expected_names_to_decl[long_name] = m;
    }
  });

  ret.compilation_units = get_compilation_units(path);
  for (auto& unit : ret.compilation_units) {
    unit.populate_functions(expected_names_to_decl);
    for (auto& function : unit.get_functions()) {
      auto name = function.get_name();
      always_assert_log(ret.name_to_function.count(name) == 0,
                        "Duplicate symbol {%s} in native code!",
                        name.c_str());
      ret.name_to_function[name] = &function;
      auto java_declaration = function.get_java_declaration();
      if (java_declaration) {
        always_assert_log(
            ret.java_declaration_to_function.count(java_declaration) == 0,
            "More than one implementation for native Java method {%s}!",
            SHOW(java_declaration));

        ret.java_declaration_to_function[java_declaration] = &function;
      }
    }
  }
  return ret;
}

} // namespace native
