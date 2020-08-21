/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FbjniMarker.h"

#include "Debug.h"
#include "DexMemberRefs.h"
#include "DexUtil.h"
#include "JavaParserUtil.h"

#include <deque>
#include <json/json.h>

template <class DexMember>
static void mark_member_reachable_by_native(DexMember* member) {
  member->rstate.set_keepnames(keep_reason::NATIVE);
}

static void mark_class_reachable_by_native(const DexType* dtype) {
  auto dclass = type_class_internal(dtype);
  always_assert_log(
      dclass != nullptr, "Could not resolve type %s", show(dtype).c_str());

  mark_member_reachable_by_native(dclass);
}

DexType* FbjniMarker::process_class_path(const std::string& class_path) {
  std::string class_name = java_names::external_to_internal(class_path);

  auto type = DexType::get_type(class_name.c_str());
  always_assert_log(
      type != nullptr, "Could not resolve type %s", class_path.c_str());

  // keep declared type
  types.insert(type);

  mark_class_reachable_by_native(type);
  return type;
}

DexField* FbjniMarker::process_field(DexType* type,
                                     const std::string& field_str) {
  auto field_tokens = java_declarations::parse_field_declaration(field_str);

  auto field_tokens_internal = dex_member_refs::FieldDescriptorTokens();
  field_tokens_internal.cls = type->str();
  field_tokens_internal.name = field_tokens.name;
  field_tokens_internal.type = to_internal_type(field_tokens.type);

  auto field_ref = DexField::get_field(field_tokens_internal);
  always_assert_log(
      field_ref != nullptr, "Could not resolve field %s", field_str.c_str());

  auto field = field_ref->as_def();
  always_assert_log(
      field != nullptr, "Field %s is not a definition", field_str.c_str());

  mark_member_reachable_by_native(field);
  return field;
}

DexMethod* FbjniMarker::process_method(DexType* type,
                                       const std::string& method_str) {
  auto method_tokens = java_declarations::parse_method_declaration(method_str);

  if (method_tokens.rtype.empty()) {
    // is constructor
    method_tokens.rtype = "void";
    method_tokens.name = "<init>";
  }

  auto method_tokens_internal = dex_member_refs::MethodDescriptorTokens();
  method_tokens_internal.cls = type->str();
  method_tokens_internal.name = method_tokens.name;
  method_tokens_internal.rtype = to_internal_type(method_tokens.rtype);
  for (auto str : method_tokens.args) {
    method_tokens_internal.args.push_back(to_internal_type(str));
  }

  auto method_ref = DexMethod::get_method(method_tokens_internal);
  always_assert_log(method_ref != nullptr,
                    "Could not resolve method: %s",
                    method_str.c_str());

  auto method = method_ref->as_def();
  always_assert_log(
      method != nullptr, "Method %s is not a definition", method_str.c_str());

  mark_member_reachable_by_native(method);
  return method;
}

std::string FbjniMarker::to_internal_type(const std::string& str) {
  int array_level = std::count(str.begin(), str.end(), '[');
  std::string array_prefix;
  for (int i = 0; i < array_level; i++) {
    array_prefix += '[';
  }

  std::string type_str = array_level > 0 ? str.substr(0, str.find('[')) : str;
  if (java_names::primitive_name_to_desc(type_str)) {
    // is primitive type
    auto internal_name = java_names::external_to_internal(type_str);
    return array_prefix + internal_name;
  } else {
    // not primitive, try fully-qualify name first
    auto inter_str = java_names::external_to_internal(type_str);
    for (auto dtype : types) {
      if (dtype->str() == type_str) {
        return array_prefix + type_str;
      }
    }

    // try to match simple name (more common)
    for (auto dtype : types) {
      if (type::get_simple_name(dtype) == type_str) {
        return array_prefix + dtype->str();
      }
    }

    // error: No matching type
    not_reached_log("Can not resolve type %s", str.c_str());
    return nullptr;
  }
}

void mark_native_classes_from_fbjni_configs(
    const std::vector<std::string>& json_files) {
  FbjniMarker marker = FbjniMarker();

  for (const std::string& json_file : json_files) {
    std::ifstream config_stream(json_file);
    if (!config_stream) {
      fprintf(stderr, "error: cannot find file: %s\n", json_file.c_str());
      exit(EXIT_FAILURE);
    }
    Json::Value json;
    config_stream >> json;
    for (Json::Value::ArrayIndex i = 0; i < json.size(); i++) {
      auto type = marker.process_class_path(json[i]["class_path"].asString());

      Json::Value& fields = json[i]["fields"];
      for (Json::Value::ArrayIndex j = 0; j < fields.size(); j++) {
        marker.process_field(type, fields[j].asString());
      }

      Json::Value& methods = json[i]["methods"];
      for (Json::Value::ArrayIndex j = 0; j < methods.size(); j++) {
        marker.process_method(type, methods[j].asString());
      }

      Json::Value& exceptions = json[i]["exceptions"];
      for (Json::Value::ArrayIndex j = 0; j < exceptions.size(); j++) {
        marker.process_class_path(exceptions[j].asString());
      }
    }
  }
}
