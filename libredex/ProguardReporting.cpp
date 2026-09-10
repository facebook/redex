/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardReporting.h"

#include <json/value.h>
#include <json/writer.h>

#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <unordered_map>

#include "Debug.h"
#include "DexClass.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "KeepReason.h"
#include "MethodUtil.h"
#include "ProguardConfiguration.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "ProguardPrintConfiguration.h"
#include "Show.h"

std::string_view extract_suffix(std::string_view class_name) {
  auto i = class_name.find_last_of('.');
  if (i == std::string::npos) {
    // This is a class name with no package prefix.
    return class_name;
  }
  return class_name.substr(i + 1);
}

std::string type_descriptor_to_java(const std::string& descriptor) {
  redex_assert(!descriptor.empty());
  if (descriptor[0] == '[') {
    return type_descriptor_to_java(descriptor.substr(1)) + "[]";
  }
  if (descriptor == "B") {
    return "byte";
  }
  if (descriptor == "S") {
    return "short";
  }
  if (descriptor == "I") {
    return "int";
  }
  if (descriptor == "J") {
    return "long";
  }
  if (descriptor == "C") {
    return "char";
  }
  if (descriptor == "F") {
    return "float";
  }
  if (descriptor == "D") {
    return "double";
  }
  if (descriptor == "Z") {
    return "boolean";
  }
  if (descriptor == "V") {
    return "void";
  }
  if (descriptor[0] == 'L') {
    return java_names::internal_to_external(descriptor);
  }
  std::cerr << "type_descriptor_to_java: unexpected type descriptor "
            << descriptor << '\n';
  exit(2);
}

std::string_view extract_member_name(const std::string_view qualified) {
  auto dot = qualified.find('.');
  auto colon = qualified.find(':');
  return qualified.substr(dot + 1, colon - dot - 1);
}

// Convert a type descriptor that may contain obfuscated class names
// into the corresponding type descriptor with the class types deobfuscated.
// The incoming type descriptor is a chain of types which may be primitive
// types, array types or class types. For example [[A; -> [[Lcom.wombat.Numbat;
std::string deobfuscate_type_descriptor(const ProguardMap& pg_map,
                                        const std::string& desc) {
  redex_assert(!desc.empty());
  std::string deob;
  size_t i = 0;
  while (i < desc.size()) {
    if (desc[i] == 'L') {
      auto colon = desc.find(';');
      redex_assert(colon != std::string::npos);
      auto class_type = desc.substr(i, colon + 1);
      auto deob_class = pg_map.deobfuscate_class(class_type);
      if (deob_class.empty()) {
        std::cerr << "Warning: failed to deobfuscate class " << class_type
                  << '\n';
        deob_class = class_type;
      }
      deob += deob_class;
      i = colon + 1;
      continue;
    }
    deob += desc[i];
    i++;
  }
  return deob;
}

std::string form_java_args(const ProguardMap& pg_map, const DexTypeList* args) {
  std::string s;
  unsigned long i = 0;
  for (const auto& arg : *args) {
    const auto* desc = arg->get_name()->c_str();
    auto deobfu_desc = deobfuscate_type_descriptor(pg_map, desc);
    s += type_descriptor_to_java(deobfu_desc);
    if (i < args->size() - 1) {
      s += ",";
    }
    i++;
  }
  return s;
}

std::string java_args(const ProguardMap& pg_map, const DexTypeList* args) {
  std::string str = "(";
  str += form_java_args(pg_map, args);
  str += ")";
  return str;
}

void redex::print_method(std::ostream& output,
                         const ProguardMap& pg_map,
                         const std::string& class_name,
                         const DexMethod* method) {
  std::string_view method_name = extract_member_name(method->get_name()->str());
  // Record if this is a constructor to suppress return value printing
  // before the method name.
  bool is_constructor = method::is_init(method);
  if (is_constructor) {
    method_name = extract_suffix(class_name);
    is_constructor = true;
  } else {
    const auto deob = method->get_deobfuscated_name_or_empty();
    if (deob.empty()) {
      std::cerr << "WARNING: method has no deobfu: " << method_name << '\n';
    } else {
      method_name = extract_member_name(deob);
    }
  }
  auto* proto = method->get_proto();
  auto* args = proto->get_args();
  auto* return_type = proto->get_rtype();
  output << class_name << ": ";
  if (!is_constructor) {
    const auto* return_type_desc = return_type->get_name()->c_str();
    auto deobfu_return_type =
        deobfuscate_type_descriptor(pg_map, return_type_desc);
    output << type_descriptor_to_java(deobfu_return_type) << " ";
  }
  output << method_name << java_args(pg_map, args) << '\n';
}

template <class Container>
void redex::print_methods(std::ostream& output,
                          const ProguardMap& pg_map,
                          const std::string& class_name,
                          const Container& methods) {
  for (const DexMethod* method : methods) {
    redex::print_method(output, pg_map, class_name, method);
  }
}

void redex::print_field(std::ostream& output,
                        const ProguardMap& pg_map,
                        const std::string& class_name,
                        const DexField* field) {
  const auto* field_type = field->get_type()->get_name()->c_str();
  std::string deobfu_field_type =
      deobfuscate_type_descriptor(pg_map, field_type);
  output << class_name << ": " << type_descriptor_to_java(deobfu_field_type)
         << " " << extract_member_name(field->get_deobfuscated_name_or_empty())
         << '\n';
}

template <class Container>
void redex::print_fields(std::ostream& output,
                         const ProguardMap& pg_map,
                         const std::string& class_name,
                         const Container& fields) {
  for (const DexField* field : fields) {
    redex::print_field(output, pg_map, class_name, field);
  }
}

void redex::print_class(std::ostream& output,
                        const ProguardMap& pg_map,
                        const DexClass* cls) {
  const auto& deob_name = [&]() {
    const auto& deob = cls->get_deobfuscated_name_or_empty();
    if (!deob.empty()) {
      return deob;
    }
    std::cerr << "WARNING: this class has no deobfuscated name: "
              << cls->get_name()->c_str() << '\n';
    return cls->get_name()->str();
  }();
  std::string name = java_names::internal_to_external(deob_name);
  output << name << '\n';
  print_fields(output, pg_map, name, cls->get_ifields());
  print_fields(output, pg_map, name, cls->get_sfields());
  print_methods(output, pg_map, name, cls->get_dmethods());
  print_methods(output, pg_map, name, cls->get_vmethods());
}

void redex::print_classes(std::ostream& output,
                          const ProguardMap& pg_map,
                          const Scope& classes) {
  for (const auto& cls : classes) {
    if (!cls->is_external()) {
      redex::print_class(output, pg_map, cls);
    }
  }
}

namespace {

std::string proguard_rule_kind(const keep_rules::KeepSpec& keep_rule) {
  auto keep_style = keep_rules::show_keep_style(keep_rule);
  if (!keep_style.empty() && keep_style[0] == '-') {
    keep_style.erase(0, 1);
  }
  return keep_style;
}

void append_proguard_rule_warnings(Json::Value& warnings,
                                   int rule_id,
                                   const keep_rules::KeepSpec& keep_rule) {
  auto append_warning = [&](const std::string& message) {
    Json::Value warning;
    warning["rule_id"] = rule_id;
    warning["source"] =
        keep_rule.source_filename + ":" + std::to_string(keep_rule.source_line);
    warning["message"] = message;
    warnings.append(warning);
  };
  if (keep_rule.allowoptimization) {
    append_warning("allowoptimization is parsed but not implemented by Redex");
  }
}

Json::Value make_proguard_parser_diagnostic_json(
    const keep_rules::proguard_parser::CommandDiagnostic& diagnostic) {
  Json::Value value;
  value["command"] = diagnostic.command;
  value["source_filename"] = diagnostic.filename;
  value["source_line"] = diagnostic.line;
  value["source"] = diagnostic.filename + ":" + std::to_string(diagnostic.line);
  value["context"] = diagnostic.context;
  return value;
}

Json::Value make_proguard_rule_json(
    int rule_id,
    const keep_rules::KeepSpec& keep_rule,
    const std::string& kind,
    const std::string& text,
    const keep_rules::ProguardRuleRecorder* recorder,
    const ConcurrentSet<const keep_rules::KeepSpec*>* used_rules,
    const ConcurrentSet<const keep_rules::KeepSpec*>* unused_rules) {
  Json::Value rule;
  rule["id"] = rule_id;
  rule["kind"] = kind;
  rule["text"] = text;
  rule["source_filename"] = keep_rule.source_filename;
  rule["source_line"] = keep_rule.source_line;
  rule["source"] =
      keep_rule.source_filename + ":" + std::to_string(keep_rule.source_line);
  rule["mark_classes"] = keep_rule.mark_classes;
  rule["mark_conditionally"] = keep_rule.mark_conditionally;
  rule["allowshrinking"] = keep_rule.allowshrinking;
  rule["allowoptimization"] = keep_rule.allowoptimization;
  rule["allowobfuscation"] = keep_rule.allowobfuscation;
  rule["includedescriptorclasses"] = keep_rule.includedescriptorclasses;
  if (recorder != nullptr) {
    rule["matched"] = used_rules->count(&keep_rule) != 0u;
    rule["unused"] = unused_rules->count(&keep_rule) != 0u;
  }
  return rule;
}

template <class DexMember>
Json::Value make_proguard_lens_item_json(
    const char* kind,
    const std::string& name,
    const DexMember* member,
    const std::unordered_map<const keep_rules::KeepSpec*, int>& rule_ids) {
  Json::Value item;
  item["kind"] = kind;
  item["name"] = name;
  auto current_name = show(member);
  item["current_name"] = current_name;
  item["renamed"] = current_name != name;
  item["referenced_state"] = member->rstate.str();
  item["present"] = true;
  item["can_delete"] = member->rstate.can_delete();
  item["can_rename"] = member->rstate.can_rename();
  if (keep_reason::Reason::record_keep_reasons()) {
    Json::Value reason_ids(Json::arrayValue);
    Json::Value reasons(Json::arrayValue);
    for (const auto* reason :
         UnorderedIterable(member->rstate.keep_reasons())) {
      std::ostringstream reason_text;
      reason_text << *reason;
      reasons.append(reason_text.str());
      if (reason->type == keep_reason::KEEP_RULE) {
        auto it = rule_ids.find(reason->keep_rule);
        if (it != rule_ids.end()) {
          reason_ids.append(it->second);
        }
      }
    }
    item["reason_ids"] = reason_ids;
    item["reasons"] = reasons;
  }
  return item;
}

} // namespace

void redex::dump_proguard_lens_json(
    const std::string& path,
    const DexStoresVector& stores,
    const char* phase,
    const keep_rules::ProguardConfiguration* pg_config,
    const keep_rules::ProguardRuleRecorder* recorder,
    const keep_rules::proguard_parser::Stats* parser_stats,
    const keep_rules::proguard_parser::Diagnostics* diagnostics,
    size_t blocklisted_rules) {
  Json::Value root;
  root["schema_version"] = 1;
  root["phase"] = phase;
  root["rules"] = Json::arrayValue;
  root["items"] = Json::arrayValue;
  root["warnings"] = Json::arrayValue;

  std::unordered_map<const keep_rules::KeepSpec*, int> rule_ids;
  if (pg_config != nullptr) {
    int rule_id = 1;
    for (const auto& keep_rule : pg_config->keep_rules) {
      rule_ids[keep_rule] = rule_id;
      root["rules"].append(make_proguard_rule_json(
          rule_id,
          *keep_rule,
          proguard_rule_kind(*keep_rule),
          keep_rules::show_keep(*keep_rule),
          recorder,
          recorder != nullptr ? &recorder->used_keep_rules : nullptr,
          recorder != nullptr ? &recorder->unused_keep_rules : nullptr));
      append_proguard_rule_warnings(root["warnings"], rule_id, *keep_rule);
      rule_id++;
    }
    for (const auto& keep_rule : pg_config->assumenosideeffects_rules) {
      root["rules"].append(make_proguard_rule_json(
          rule_id,
          *keep_rule,
          "assumenosideeffects",
          keep_rules::show_simple_keep_rule(*keep_rule, "-assumenosideeffects"),
          recorder,
          recorder != nullptr ? &recorder->used_assumenosideeffect_rules
                              : nullptr,
          recorder != nullptr ? &recorder->unused_assumenosideeffect_rules
                              : nullptr));
      append_proguard_rule_warnings(root["warnings"], rule_id, *keep_rule);
      rule_id++;
    }
    for (const auto& keep_rule : pg_config->assumevalues_rules) {
      root["rules"].append(make_proguard_rule_json(
          rule_id,
          *keep_rule,
          "assumevalues",
          keep_rules::show_simple_keep_rule(*keep_rule, "-assumevalues"),
          recorder,
          recorder != nullptr ? &recorder->used_assumevalues_rules : nullptr,
          recorder != nullptr ? &recorder->unused_assumevalues_rules
                              : nullptr));
      append_proguard_rule_warnings(root["warnings"], rule_id, *keep_rule);
      rule_id++;
    }
  }

  if (parser_stats != nullptr) {
    root["parse_stats"]["parse_errors"] =
        Json::Value::UInt64(parser_stats->parse_errors);
    root["parse_stats"]["unknown_tokens"] =
        Json::Value::UInt64(parser_stats->unknown_tokens);
    root["parse_stats"]["unimplemented"] =
        Json::Value::UInt64(parser_stats->unimplemented);
    root["parse_stats"]["unknown_commands"] =
        Json::Value::UInt64(parser_stats->unknown_commands);
    root["parse_stats"]["blocklisted_rules"] =
        Json::Value::UInt64(blocklisted_rules);
    const auto* skipped_commands =
        diagnostics != nullptr ? &diagnostics->skipped_commands : nullptr;
    const auto* unknown_commands =
        diagnostics != nullptr ? &diagnostics->unknown_commands : nullptr;

    root["parse_stats"]["skipped_commands"] = Json::arrayValue;
    if (skipped_commands != nullptr) {
      for (const auto& diagnostic : *skipped_commands) {
        Json::Value diagnostic_json =
            make_proguard_parser_diagnostic_json(diagnostic);
        root["parse_stats"]["skipped_commands"].append(diagnostic_json);

        Json::Value warning = diagnostic_json;
        warning["message"] = "ProGuard command skipped by Redex's parser";
        root["warnings"].append(warning);
      }
    }
    root["parse_stats"]["unknown_command_details"] = Json::arrayValue;
    if (unknown_commands != nullptr) {
      for (const auto& diagnostic : *unknown_commands) {
        Json::Value diagnostic_json =
            make_proguard_parser_diagnostic_json(diagnostic);
        root["parse_stats"]["unknown_command_details"].append(diagnostic_json);

        Json::Value warning = diagnostic_json;
        warning["message"] = "unknown ProGuard command ignored by Redex";
        root["warnings"].append(warning);
      }
    }
    if (parser_stats->unimplemented > 0 &&
        (skipped_commands == nullptr || skipped_commands->empty())) {
      Json::Value warning;
      warning["message"] =
          "one or more ProGuard commands were skipped by Redex's parser";
      warning["count"] = Json::Value::UInt64(parser_stats->unimplemented);
      root["warnings"].append(warning);
    }
    if (parser_stats->unknown_commands > 0 &&
        (unknown_commands == nullptr || unknown_commands->empty())) {
      Json::Value warning;
      warning["message"] =
          "one or more unknown ProGuard commands were ignored by Redex";
      warning["count"] = Json::Value::UInt64(parser_stats->unknown_commands);
      root["warnings"].append(warning);
    }
  }

  for (const auto& store : stores) {
    for (const auto& dex : store.get_dexen()) {
      for (const auto* cls : dex) {
        root["items"].append(make_proguard_lens_item_json(
            "class", show_deobfuscated(cls), cls, rule_ids));
        for (const auto* method : cls->get_all_methods()) {
          root["items"].append(make_proguard_lens_item_json(
              "method", show_deobfuscated(method), method, rule_ids));
        }
        for (const auto* field : cls->get_all_fields()) {
          root["items"].append(make_proguard_lens_item_json(
              "field", show_deobfuscated(field), field, rule_ids));
        }
      }
    }
  }

  std::ofstream ofs(path);
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  ofs << Json::writeString(builder, root) << "\n";
}
