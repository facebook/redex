/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AnnoKill.h"

#include <map>
#include <stdio.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_ANNO_KILLED = "num_anno_killed";
constexpr const char* METRIC_ANNO_TOTAL = "num_anno_total";
constexpr const char* METRIC_CLASS_ASETS_CLEARED = "num_class_cleared";
constexpr const char* METRIC_CLASS_ASETS_TOTAL = "num_class_total";
constexpr const char* METRIC_METHOD_ASETS_CLEARED = "num_method_cleared";
constexpr const char* METRIC_METHOD_ASETS_TOTAL = "num_method_total";
constexpr const char* METRIC_METHODPARAM_ASETS_CLEARED = "num_methodparam_cleared";
constexpr const char* METRIC_METHODPARAM_ASETS_TOTAL = "num_methodparam_total";
constexpr const char* METRIC_FIELD_ASETS_CLEARED = "num_field_cleared";
constexpr const char* METRIC_FIELD_ASETS_TOTAL = "num_field_total";

struct kill_counters {
  int annotations;
  int annotations_killed;
  int class_asets;
  int class_asets_cleared;
  int method_asets;
  int method_asets_cleared;
  int method_param_asets;
  int method_param_asets_cleared;
  int field_asets;
  int field_asets_cleared;
};

kill_counters s_kcount;

static std::map<std::string, size_t> s_build_anno_map;
static std::map<std::string, size_t> s_runtime_anno_map;
static std::map<std::string, size_t> s_system_anno_map;
static size_t s_build_count = 0;
static size_t s_runtime_count = 0;
static size_t s_system_count = 0;

void count_annotation(const DexAnnotation* da) {
  std::string annoName = da->type()->get_name()->c_str();
  if (da->system_visible()) {
      s_system_anno_map[annoName]++;
      s_system_count++;
  } else if (da->runtime_visible()) {
      s_runtime_anno_map[annoName]++;
      s_runtime_count++;
  } else if (da->build_visible()) {
      s_build_anno_map[annoName]++;
      s_build_count++;
  }
}

void cleanup_aset(DexAnnotationSet* aset,
    const std::unordered_set<DexType*>& keep_annos,
    const std::unordered_set<DexType*>& anno_refs_in_code) {
  s_kcount.annotations += aset->size();
  auto& annos = aset->get_annotations();
  auto iter = annos.begin();
  while (iter != annos.end()) {
    auto tokill = iter;
    DexAnnotation* da = *iter++;
    auto anno_type = da->type();

    count_annotation(da);

    if (anno_refs_in_code.count(anno_type) > 0) {
      TRACE(ANNO, 3,
          "Annotation type %s with type referenced in code, skipping...\n\tannotation: %s\n",
          SHOW(anno_type), SHOW(da));
      continue;
    }
    if (keep_annos.count(anno_type) > 0) {
      TRACE(ANNO, 3,
          "Blacklisted annotation type %s, skipping...\n\tannotation: %s\n",
          SHOW(anno_type), SHOW(da));
      continue;
    }

    if (!da->system_visible()) {
      TRACE(ANNO, 3, "Killing annotation %s\n", SHOW(da));
      annos.erase(tokill);
      s_kcount.annotations_killed++;
      delete da;
    }
  }
}

void kill_annotations(const std::vector<DexClass*>& classes,
    const std::unordered_set<DexType*>& keep_annos,
    const std::unordered_set<DexType*>& anno_refs_in_code) {
  for (auto clazz : classes) {
    DexAnnotationSet* aset = clazz->get_anno_set();
    if (aset == nullptr) continue;
    s_kcount.class_asets++;
    cleanup_aset(aset, keep_annos, anno_refs_in_code);
    if (aset->size() == 0) {
      TRACE(ANNO, 3, "Clearing annotation for class %s\n", SHOW(clazz->get_type()));
      clazz->clear_annotations();
      s_kcount.class_asets_cleared++;
    }
  }
  walk_methods(classes,
      [&](DexMethod* method) {
        DexAnnotationSet* aset = method->get_anno_set();
        if (aset == nullptr) return;
        s_kcount.method_asets++;
        cleanup_aset(aset, keep_annos, anno_refs_in_code);
        if (aset->size() == 0) {
          TRACE(ANNO, 3, "Clearing annotations for method %s.%s:%s\n",
              SHOW(method->get_class()), SHOW(method->get_name()), SHOW(method->get_proto()));
          method->clear_annotations();
          s_kcount.method_asets_cleared++;
        }
      });
  walk_methods(classes,
      [&](DexMethod* method) {
        /* Parameter annotations... */
        auto pas = method->get_param_anno();
        if (pas == nullptr) return;
        s_kcount.method_param_asets += pas->size();
        bool clear_pas = true;
        for (auto pa : *pas) {
          DexAnnotationSet* aset = pa.second;
          if (aset->size() == 0) continue;
          cleanup_aset(aset, keep_annos, anno_refs_in_code);
          if (aset->size() == 0) continue;
          clear_pas = false;
        }
        if (clear_pas) {
          TRACE(ANNO, 3, "Clearing parameter annotations for method parameters %s.%s:%s\n",
              SHOW(method->get_class()), SHOW(method->get_name()), SHOW(method->get_proto()));
          s_kcount.method_param_asets_cleared += pas->size();
          for (auto pa : *pas) {
            delete pa.second;
          }
          pas->clear();
        }
      });
  walk_fields(classes,
      [&](DexField* field) {
        DexAnnotationSet* aset = field->get_anno_set();
        if (aset == nullptr) return;
        s_kcount.field_asets++;
        cleanup_aset(aset, keep_annos, anno_refs_in_code);
        if (aset->size() == 0) {
          TRACE(ANNO, 3, "Clearing annotations for field %s.%s:%s\n",
              SHOW(field->get_class()), SHOW(field->get_name()), SHOW(field->get_type()));
          field->clear_annotations();
          s_kcount.field_asets_cleared++;
        }
      });
}

void referenced_annos(const Scope& scope,
    const std::unordered_set<DexType*>& annotations,
    std::unordered_set<DexType*>& referenced_annos) {

  // mark an annotation as "unremovable" if a field is typed with that annotation
  walk_fields(scope,
      [&](DexField* field) {
        // don't look at fields defined on the annotation itself
        const auto field_cls_type = field->get_class();
        if (annotations.count(field_cls_type) > 0) return;
        const auto field_cls = type_class(field_cls_type);
        if (field_cls != nullptr && is_annotation(field_cls)) return;

        auto ftype = field->get_type();
        if (annotations.count(ftype) > 0) {
          TRACE(ANNO, 3, "Field typed with an annotation type %s.%s:%s\n",
              SHOW(field->get_class()), SHOW(field->get_name()), SHOW(ftype));
          referenced_annos.insert(ftype);
        }
      });

  // mark an annotation as "unremovable" if a method signature contains a type with that annotation
  walk_methods(scope,
      [&](DexMethod* meth) {
        // don't look at methods defined on the annotation itself
        const auto meth_cls_type = meth->get_class();
        if (annotations.count(meth_cls_type) > 0) return;
        const auto meth_cls = type_class(meth_cls_type);
        if (meth_cls != nullptr && is_annotation(meth_cls)) return;

        const auto has_anno = [&](DexType* type) {
          if (annotations.count(type) > 0) {
            TRACE(ANNO, 3, "Method contains annotation type in signature %s.%s:%s\n",
                SHOW(meth->get_class()), SHOW(meth->get_name()), SHOW(meth->get_proto()));
            referenced_annos.insert(type);
          }
        };

        const auto proto = meth->get_proto();
        has_anno(proto->get_rtype());
        for (const auto& arg : proto->get_args()->get_type_list()) {
          has_anno(arg);
        }
      });

  // mark an annotation as "unremovable" if any opecode refrences the annotation type
  walk_opcodes(scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* meth, DexInstruction* insn) {
        // don't look at methods defined on the annotation itself
        const auto meth_cls_type = meth->get_class();
        if (annotations.count(meth_cls_type) > 0) return;
        const auto meth_cls = type_class(meth_cls_type);
        if (meth_cls != nullptr && is_annotation(meth_cls)) return;

        if (insn->has_types()) {
          auto type = static_cast<DexOpcodeType*>(insn)->get_type();
          if (annotations.count(type) > 0) {
            referenced_annos.insert(type);
            TRACE(ANNO, 3, "Annotation referenced in type opcode\n\t%s.%s:%s - %s\n",
                SHOW(meth->get_class()), SHOW(meth->get_name()), SHOW(meth->get_proto()),
                SHOW(insn));
          }
        } else if (insn->has_fields()) {
          auto field = static_cast<DexOpcodeField*>(insn)->field();
          auto fdef = resolve_field(field,
              is_sfield_op(insn->opcode()) ? FieldSearch::Static : FieldSearch::Instance);
          if (fdef != nullptr) field = fdef;

          bool referenced = false;
          auto owner = field->get_class();
          if (annotations.count(owner) > 0) {
            referenced = true;
            referenced_annos.insert(owner);
          }
          auto type = field->get_type();
          if (annotations.count(type) > 0) {
            referenced = true;
            referenced_annos.insert(type);
          }
          if (referenced) {
            TRACE(ANNO, 3, "Annotation referenced in field opcode\n\t%s.%s:%s - %s\n",
                SHOW(meth->get_class()), SHOW(meth->get_name()), SHOW(meth->get_proto()),
                SHOW(insn));
          }
        } else if (insn->has_methods()) {
          auto method = static_cast<DexOpcodeMethod*>(insn)->get_method();
          DexMethod* methdef;
          switch (insn->opcode()) {
            case OPCODE_INVOKE_INTERFACE:
            case OPCODE_INVOKE_INTERFACE_RANGE:
              methdef = resolve_intf_methodref(method);
              break;
            case OPCODE_INVOKE_VIRTUAL:
            case OPCODE_INVOKE_VIRTUAL_RANGE:
              methdef = resolve_method(method, MethodSearch::Virtual);
              break;
            case OPCODE_INVOKE_STATIC:
            case OPCODE_INVOKE_STATIC_RANGE:
              methdef = resolve_method(method, MethodSearch::Static);
              break;
            case OPCODE_INVOKE_DIRECT:
            case OPCODE_INVOKE_DIRECT_RANGE:
              methdef = resolve_method(method, MethodSearch::Direct);
              break;
            default:
              methdef = resolve_method(method, MethodSearch::Any);
          }
          if (methdef != nullptr) method = methdef;

          bool referenced = false;
          auto owner = method->get_class();
          if (annotations.count(owner) > 0) {
            referenced = true;
            referenced_annos.insert(owner);
          }
          auto proto = method->get_proto();
          auto rtype = proto->get_rtype();
          if (annotations.count(rtype) > 0) {
            referenced = true;
            referenced_annos.insert(rtype);
          }
          auto arg_list = proto->get_args();
          for (const auto& arg : arg_list->get_type_list()) {
            if (annotations.count(arg) > 0) {
              referenced = true;
              referenced_annos.insert(arg);
            }
          }
          if (referenced) {
            TRACE(ANNO, 3, "Annotation referenced in method opcode\n\t%s.%s:%s - %s\n",
                SHOW(meth->get_class()), SHOW(meth->get_name()), SHOW(meth->get_proto()),
                SHOW(insn));
          }
        }
      });
}

void gather_annos(const Scope& scope, std::unordered_set<DexType*>& annotations) {
  // all classes marked as annotation
  for (const auto& cls : scope) {
    if (!is_annotation(cls)) continue;
    annotations.insert(cls->get_type());
  }

  // all used annotations
  auto annos_in_aset = [&](DexAnnotationSet* aset) {
    if (aset == nullptr) return;
    for (const auto& anno : aset->get_annotations()) {
      annotations.insert(anno->type());
    }
  };
  // all annotations in classes
  for (auto cls : scope) {
    annos_in_aset(cls->get_anno_set());
  }
  // all annotations in methods
  walk_methods(
      scope,
      [&](DexMethod* method) {
        annos_in_aset(method->get_anno_set());
        auto pas = method->get_param_anno();
        if (pas == nullptr) return;
        for (auto pa : *pas) {
          annos_in_aset(pa.second);
        }
      });
  // all annotations in fields
  walk_fields(
      scope,
      [&](DexField* field) {
        annos_in_aset(field->get_anno_set());
      });
}

}

void AnnoKillPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  // load annotations that should not be deleted
  std::unordered_set<DexType*> keep_annos;
  TRACE(ANNO, 2, "Keep annotations count %d\n", m_keep_annos.size());
  for (const auto& anno_name : m_keep_annos) {
    auto anno_type = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Keep annotation type string %s\n", anno_name.c_str());
    if (anno_type != nullptr) {
      TRACE(ANNO, 2, "Keep annotation type %s\n", SHOW(anno_type));
      keep_annos.insert(anno_type);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s\n", anno_name.c_str());
    }
  }

  auto scope = build_class_scope(stores);

  // find all annotations classes in scope and all annotations used in scope
  std::unordered_set<DexType*> annotations;
  gather_annos(scope, annotations);
  // find all annotations referenced by code
  std::unordered_set<DexType*> anno_refs_in_code;
  referenced_annos(scope, annotations, anno_refs_in_code);

  kill_annotations(scope, keep_annos, anno_refs_in_code);
  TRACE(ANNO, 1, "AnnoKill report killed/total\n");
  TRACE(ANNO, 1,
      "Annotations: %d/%d\n",
      s_kcount.annotations_killed,
      s_kcount.annotations);
  TRACE(ANNO, 1,
      "Class Asets: %d/%d\n",
      s_kcount.class_asets_cleared,
      s_kcount.class_asets);
  TRACE(ANNO, 1,
      "Method Asets: %d/%d\n",
      s_kcount.method_asets_cleared,
      s_kcount.method_asets);
  TRACE(ANNO, 1,
      "MethodParam Asets: %d/%d\n",
      s_kcount.method_param_asets_cleared,
      s_kcount.method_param_asets);
  TRACE(ANNO, 1,
      "Field Asets: %d/%d\n",
      s_kcount.field_asets_cleared,
      s_kcount.field_asets);

  TRACE(ANNO, 3, "Total referenced Build Annos: %d\n", s_build_count);
  TRACE(ANNO, 3, "Total referenced Runtime Annos: %d\n", s_runtime_count);
  TRACE(ANNO, 3, "Total referenced System Annos: %d\n", s_system_count);

  for (const auto& p : s_build_anno_map) {
    TRACE(ANNO, 3, "Build anno: %lu, %s\n", p.second, p.first.c_str());
  }

  for (const auto& p : s_runtime_anno_map) {
    TRACE(ANNO, 3, "Runtime anno: %lu, %s\n", p.second, p.first.c_str());
  }

  for (const auto& p : s_system_anno_map) {
    TRACE(ANNO, 3, "System anno: %lu, %s\n", p.second, p.first.c_str());
  }

  mgr.incr_metric(METRIC_ANNO_KILLED,
                  s_kcount.annotations_killed);
  mgr.incr_metric(METRIC_ANNO_TOTAL,
                  s_kcount.annotations);
  mgr.incr_metric(METRIC_CLASS_ASETS_CLEARED,
                  s_kcount.class_asets_cleared);
  mgr.incr_metric(METRIC_CLASS_ASETS_TOTAL,
                  s_kcount.class_asets);
  mgr.incr_metric(METRIC_METHOD_ASETS_CLEARED,
                  s_kcount.method_asets_cleared);
  mgr.incr_metric(METRIC_METHOD_ASETS_TOTAL,
                  s_kcount.method_asets);
  mgr.incr_metric(METRIC_METHODPARAM_ASETS_CLEARED,
                  s_kcount.method_param_asets_cleared);
  mgr.incr_metric(METRIC_METHODPARAM_ASETS_TOTAL,
                  s_kcount.method_param_asets);
  mgr.incr_metric(METRIC_FIELD_ASETS_CLEARED,
                  s_kcount.field_asets_cleared);
  mgr.incr_metric(METRIC_FIELD_ASETS_TOTAL,
                  s_kcount.field_asets);
}

static AnnoKillPass s_pass;
