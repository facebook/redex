/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Native.h"

#include <boost/optional.hpp>
#include <fstream>
#include <json/json.h>
#include <json/value.h>

#include "Debug.h"
#include "NativeNames.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace fs = boost::filesystem;

namespace {

boost::optional<Json::Value> read_json_from_file(const std::string& filename) {
  std::ifstream file_stream(filename);
  if (!file_stream) {
    return boost::none;
  }
  Json::Value json;
  file_stream >> json;
  file_stream.close();
  return json;
}

} // anonymous namespace

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
  auto jni_json_path = (m_infodir_path / "jni.json").string();
  auto jni_json_opt = read_json_from_file(jni_json_path);

  always_assert_log(jni_json_opt, "Cannot find file %s", jni_json_path.c_str());

  for (Json::Value::ArrayIndex i = 0; i < jni_json_opt->size(); i++) {
    std::string function_name = (*jni_json_opt)[i].asString();
    auto decl_it = expected_names_to_decl.find(function_name);
    DexMethod* decl =
        decl_it == expected_names_to_decl.end() ? nullptr : decl_it->second;
    m_functions.emplace_back(std::move(function_name), decl);
  }

  // Alternatively, native methods can be registered with RegisterNatives calls.
  // Use specific analyses to extract information.

  auto registered_natives_path =
      (m_infodir_path / "registered_natives.json").string();
  auto registered_natives_opt = read_json_from_file(registered_natives_path);

  if (registered_natives_opt) {
    for (Json::Value::ArrayIndex i = 0; i < registered_natives_opt->size();
         i++) {
      auto klass = (*registered_natives_opt)[i];
      std::string class_name = klass["class_name"].asString();
      auto methods = klass["registered_functions"];
      for (Json::Value::ArrayIndex j = 0; j < methods.size(); j++) {
        auto method = methods[j];
        std::string method_name = method["method_name"].asString();
        std::string desc = method["desc"].asString();
        std::string function = method["function"].asString();

        DexMethodRef* m =
            DexMethod::get_method(class_name + "." + method_name + ":" + desc);
        if (m) {
          DexMethod* decl = m->as_def();
          always_assert_log(decl,
                            "Attempting to bind non-concrete native method.");
          m_functions.emplace_back(std::move(function), decl);
        } else {
          TRACE(NATIVE,
                2,
                "Method %s%s%s not found in Java code.",
                class_name.c_str(),
                method_name.c_str(),
                desc.c_str());
        }
      }
    }
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
