/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TrackResources.h"

#include <cinttypes>
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
#include "IRCode.h"
#include "PassManager.h"
#include "ProguardMap.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Show.h"
#include "Timer.h"
#include "Walkers.h"

namespace {

const std::string TRACKED_RESOURCES_FILE_NAME =
    "redex-tracked-coldstart-resources.txt";

std::unordered_set<std::string> build_cls_set(
    const std::vector<std::string>& cls_list) {
  std::unordered_set<std::string> cls_set;
  for (auto& cls : cls_list) {
    cls_set.emplace(cls);
  }
  return cls_set;
}

void write_found_fields(const std::string& path,
                        std::unordered_set<DexField*>& recorded_fields) {
  if (!path.empty()) {
    TRACE(TRACKRESOURCES, 1, "Writing tracked fields to %s", path.c_str());
    FILE* fd = fopen(path.c_str(), "w");
    if (fd == nullptr) {
      perror("Error writing tracked fields file");
      return;
    }
    for (const auto& it : recorded_fields) {
      TRACE(TRACKRESOURCES, 4, "recording %s -> %s",
            SHOW(it->get_class()->get_name()), SHOW(it->get_name()));
      fprintf(fd, "%s -> %s\n", SHOW(it->get_class()->get_name()),
              SHOW(it->get_name()));
    }
    fclose(fd);
  }
}

void check_if_tracked_sget(
    DexMethod* src_method,
    DexField* target_field,
    const std::unordered_set<std::string>& src_set,
    const std::unordered_set<DexClass*>& classes_to_track,
    size_t* num_field_references,
    std::map<DexClass*, int, dexclasses_comparator>* per_cls_refs,
    std::unordered_set<DexField*>* recorded_fields) {
  auto src_cls_name = src_method->get_class()->get_name()->c_str();
  auto target_cls = type_class(target_field->get_class());
  if ((src_set.empty() || src_set.count(src_cls_name)) &&
      classes_to_track.count(target_cls) &&
      !recorded_fields->count(target_field)) {
    always_assert_log(target_field->is_concrete(), "Must be a concrete field");
    if (type::is_primitive(target_field->get_type())) {
      auto value = target_field->get_static_value();
      TRACE(TRACKRESOURCES, 3, "value %" PRIu64 ", sget to %s from %s",
            value != nullptr ? value->value() : 0, SHOW(target_field),
            SHOW(src_method));
    } else {
      TRACE(TRACKRESOURCES, 3, "(non-primitive) sget to %s from %s",
            SHOW(target_field), SHOW(src_method));
    }
    (*num_field_references)++;
    recorded_fields->emplace(target_field);
    ++(*per_cls_refs)[target_cls];
  }
}

} // namespace

size_t TrackResourcesPass::find_accessed_fields(
    const Scope& fullscope,
    const std::unordered_set<DexClass*>& classes_to_track,
    const std::unordered_set<std::string>& classes_to_search,
    std::unordered_set<DexField*>* recorded_fields) {
  Timer t("TrackResourcesPass::find_accessed_fields");
  std::unordered_set<DexField*> inline_field;
  uint32_t aflags = ACC_STATIC | ACC_FINAL;

  // data structures to track field references from given classes
  size_t num_field_references = 0;
  std::map<DexClass*, int, dexclasses_comparator> per_cls_refs;

  for (auto clazz : classes_to_track) {
    auto sfields = clazz->get_sfields();
    for (auto sfield : sfields) {
      if ((sfield->get_access() & aflags) != aflags) continue;
      inline_field.emplace(sfield);
    }
  }
  walk::opcodes(
      fullscope,
      [](DexMethod* method) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        if (insn->has_field() && opcode::is_an_sfield_op(insn->opcode())) {
          auto field = resolve_field(insn->get_field(), FieldSearch::Static);
          if (field == nullptr || !field->is_concrete()) return;
          if (inline_field.count(field) == 0) return;
          check_if_tracked_sget(method,
                                field,
                                classes_to_search,
                                classes_to_track,
                                &num_field_references,
                                &per_cls_refs,
                                recorded_fields);
        }
      });
  TRACE(TRACKRESOURCES, 1, "found %zu total sgets to tracked classes",
        num_field_references);
  for (auto& it : per_cls_refs) {
    TRACE(TRACKRESOURCES, 3, "%d sgets to %s ", it.second,
          SHOW(it.first->get_name()));
  }
  return num_field_references;
}

std::unordered_set<DexClass*> TrackResourcesPass::build_tracked_cls_set(
    const std::vector<std::string>& cls_suffixes, const ProguardMap& pg_map) {
  std::unordered_set<DexClass*> tracked_classes;
  for (auto& s : cls_suffixes) {
    auto dtype = DexType::get_type(pg_map.translate_class(s).c_str());
    if (dtype != nullptr) {
      auto cls = type_class(dtype);
      if (cls != nullptr) {
        tracked_classes.emplace(cls);
      }
    }
  }
  return tracked_classes;
}

void TrackResourcesPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  std::unordered_set<DexField*> recorded_fields;
  const auto& pg_map = conf.get_proguard_map();
  auto tracked_classes = build_tracked_cls_set(m_classes_to_track, pg_map);
  auto scope = build_class_scope(stores);
  auto coldstart_cls_map = build_cls_set(conf.get_coldstart_classes());
  auto num_field_references = find_accessed_fields(
      scope, tracked_classes, coldstart_cls_map, &recorded_fields);
  mgr.incr_metric("num_field_references", num_field_references);
  auto tracked_res = conf.metafile(TRACKED_RESOURCES_FILE_NAME);
  write_found_fields(tracked_res, recorded_fields);
}

static TrackResourcesPass s_pass;
