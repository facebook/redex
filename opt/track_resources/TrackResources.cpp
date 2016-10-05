/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TrackResources.h"

#include <stdio.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

std::unordered_set<std::string> build_cls_set(const std::vector<std::string>& cls_list) {
  std::unordered_set<std::string> cls_set;
  for (auto& cls : cls_list) {
    cls_set.emplace(cls);
  }
  return cls_set;
}

void write_found_fields(std::string path, std::set<DexField*>& recorded_fields) {
  if (!path.empty()) {
    TRACE(TRACKRESOURCES, 1, "Writing tracked fields to %s\n", path.c_str());
    FILE* fd = fopen(path.c_str(), "w");
    if (fd == nullptr) {
      perror("Error writing tracked fields file");
      return;
    }
    for (const auto &it : recorded_fields) {
      TRACE(TRACKRESOURCES, 4, "recording %s -> %s\n",
          SHOW(it->get_class()->get_name()),
          SHOW(it->get_name()));
      fprintf(fd, "%s -> %s\n",
          SHOW(it->get_class()->get_name()),
          SHOW(it->get_name()));
    }
    fclose(fd);
  }
}

void check_if_tracked_sget(DexMethod* src_method,
    DexField* target_field,
    std::unordered_set<std::string>& src_set,
    std::unordered_set<DexClass*>& classes_to_track,
    size_t& num_field_references,
    std::map<DexClass*, int>& per_cls_refs,
    std::set<DexField*>& recorded_fields) {
  auto src_cls_name = src_method->get_class()->get_name()->c_str();
  auto target_cls = type_class(target_field->get_class());
  if (src_set.count(src_cls_name) && classes_to_track.count(target_cls) && !recorded_fields.count(target_field)) {
    always_assert_log(target_field->is_concrete(), "Must be a concrete field");
    auto value = target_field->get_static_value();
    TRACE(TRACKRESOURCES, 3, "value %d, sget to %s from %s\n", value, SHOW(target_field), SHOW(src_method));
    num_field_references++;
    recorded_fields.emplace(target_field);
    if ( per_cls_refs.count(target_cls)) {
      per_cls_refs[target_cls]++;
    } else {
      per_cls_refs[target_cls] = 1;
    }
  }
}

/*
 * There's no "good way" to differentiate blank vs. non-blank
 * finals.  So, we just scan the code in the CL-init.  If
 * it's sput there, then it's a blank.  Lame, agreed, but functional.
 *
 */
void get_sput_in_clinit(DexClass* clazz,
                        std::unordered_map<DexField*, bool>& blank_statics) {
  auto methods = clazz->get_dmethods();
  for (auto method : methods) {
    if (is_clinit(method)) {
      always_assert_log(is_static(method) && is_constructor(method),
          "static constructor doesn't have the proper access bits set\n");
      auto& code = method->get_code();
      auto opcodes = code->get_instructions();
      for (auto opcode : opcodes) {
        if (opcode->has_fields() && is_sput(opcode->opcode())) {
          auto fieldop = static_cast<DexOpcodeField*>(opcode);
          auto field = resolve_field(fieldop->field(), FieldSearch::Static);
          if (field == nullptr || !field->is_concrete()) continue;
          if (field->get_class() != clazz->get_type()) continue;
          blank_statics[field] = true;
        }
      }
    }
  }
}

void find_accessed_fields(Scope& fullscope,
    ConfigFiles& cfg,
    std::unordered_set<DexClass*> classes_to_track,
    std::set<DexField*>& recorded_fields) {
  std::set<DexField*> inline_field;
  std::vector<DexClass*> scope;
  uint32_t aflags = ACC_STATIC | ACC_FINAL;

  // data structures to track field references from coldstart classes
  size_t num_field_references = 0;
  std::map<DexClass*, int> per_cls_refs;
  auto coldstart_cls_map = build_cls_set(cfg.get_coldstart_classes());

  for (auto clazz : fullscope) {
    std::unordered_map<DexField*, bool> blank_statics;
    get_sput_in_clinit(clazz, blank_statics);
    auto sfields = clazz->get_sfields();
    for (auto sfield : sfields) {
      if ((sfield->get_access() & aflags) != aflags) continue;
      if (blank_statics[sfield]) continue;
      auto value = sfield->get_static_value();
      if (value == nullptr && !is_primitive(sfield->get_type())) {
        continue;
      }
      if (value != nullptr && !value->is_evtype_primitive()) {
        continue;
      }
      inline_field.insert(sfield);
      scope.push_back(clazz);
    }
  }
  walk_opcodes(
      fullscope,
      [](DexMethod* method) { return true; },
      [&](DexMethod* method, DexInstruction* insn) {
        if (insn->has_fields() && is_sfield_op(insn->opcode())) {
          auto fieldop = static_cast<DexOpcodeField*>(insn);
          auto field = resolve_field(fieldop->field(), FieldSearch::Static);
          if (field == nullptr || !field->is_concrete()) return;
          if (inline_field.count(field) == 0) return;
          check_if_tracked_sget(method,
            field,
            coldstart_cls_map,
            classes_to_track,
            num_field_references,
            per_cls_refs,
            recorded_fields);
        }
      });
  TRACE(TRACKRESOURCES, 1,
      "found %d total sgets to tracked classes\n", num_field_references);
  for (auto& it : per_cls_refs) {
    TRACE(TRACKRESOURCES, 1,
        "%d sgets to %s \n", it.second, SHOW(it.first->get_name()));
  }
  MethodTransform::sync_all();
}

std::unordered_set<DexClass*> build_tracked_cls_set(
    std::vector<std::string>& cls_suffixes,
    const ProguardMap& pg_map) {
  std::unordered_set<DexClass*> tracked_classes;
  for (auto& s : cls_suffixes) {
    tracked_classes.emplace(
      type_class(DexType::get_type(pg_map.translate_class(s).c_str())));
  }
  return tracked_classes;
}

}

void TrackResourcesPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  std::set<DexField*> recorded_fields;
  const auto& pg_map = cfg.get_proguard_map();
  auto tracked_classes = build_tracked_cls_set(m_classes_to_track, pg_map);
  auto scope = build_class_scope(stores);
  find_accessed_fields(scope, cfg, tracked_classes, recorded_fields);
  write_found_fields(m_tracked_fields_output, recorded_fields);
}

static TrackResourcesPass s_pass;
