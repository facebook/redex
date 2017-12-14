/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AnnoKill.h"

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Walkers.h"

constexpr const char* METRIC_ANNO_KILLED = "num_anno_killed";
constexpr const char* METRIC_ANNO_TOTAL = "num_anno_total";
constexpr const char* METRIC_CLASS_ASETS_CLEARED = "num_class_cleared";
constexpr const char* METRIC_CLASS_ASETS_TOTAL = "num_class_total";
constexpr const char* METRIC_METHOD_ASETS_CLEARED = "num_method_cleared";
constexpr const char* METRIC_METHOD_ASETS_TOTAL = "num_method_total";
constexpr const char* METRIC_METHODPARAM_ASETS_CLEARED =
    "num_methodparam_cleared";
constexpr const char* METRIC_METHODPARAM_ASETS_TOTAL = "num_methodparam_total";
constexpr const char* METRIC_FIELD_ASETS_CLEARED = "num_field_cleared";
constexpr const char* METRIC_FIELD_ASETS_TOTAL = "num_field_total";

AnnoKill::AnnoKill(Scope& scope,
                   bool only_force_kill,
                   const AnnoNames& keep,
                   const AnnoNames& kill,
                   const AnnoNames& force_kill)
  : m_scope(scope), m_only_force_kill(only_force_kill) {
  // Load annotations that should not be deleted.
  TRACE(ANNO, 2, "Keep annotations count %d\n", keep.size());
  for (const auto& anno_name : keep) {
    auto anno_type = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Keep annotation type string %s\n", anno_name.c_str());
    if (anno_type) {
      TRACE(ANNO, 2, "Keep annotation type %s\n", SHOW(anno_type));
      m_keep.insert(anno_type);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s\n", anno_name.c_str());
    }
  }

  // Load annotations we know and want dead.
  for (auto const& anno_name : kill) {
    DexType* anno = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Kill annotation type string %s\n", anno_name.c_str());
    if (anno) {
      TRACE(ANNO, 2, "Kill anno: %s\n", SHOW(anno));
      m_kill.insert(anno);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s\n", anno_name.c_str());
    }
  }

  // Load annotations we know and want dead.
  for (auto const& anno_name : force_kill) {
    DexType* anno = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Force kill annotation type string %s\n", anno_name.c_str());
    if (anno) {
      TRACE(ANNO, 2, "Force kill anno: %s\n", SHOW(anno));
      m_force_kill.insert(anno);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s\n", anno_name.c_str());
    }
  }
}

AnnoKill::AnnoSet AnnoKill::get_referenced_annos() {
  AnnoKill::AnnoSet all_annos;

  // all used annotations
  auto annos_in_aset = [&](DexAnnotationSet* aset) {
    if (!aset) {
      return;
    }
    for (const auto& anno : aset->get_annotations()) {
      all_annos.insert(anno->type());
    }
  };

  for (const auto& cls : m_scope) {
    // all annotations referenced in classes
    annos_in_aset(cls->get_anno_set());

    // all classes marked as annotation
    if (is_annotation(cls)) {
      all_annos.insert(cls->get_type());
    }
  }

  // all annotations in methods
  walk_methods(m_scope, [&](DexMethod* method) {
    annos_in_aset(method->get_anno_set());
    auto param_annos = method->get_param_anno();
    if (!param_annos) {
      return;
    }
    for (auto pa : *param_annos) {
      annos_in_aset(pa.second);
    }
  });
  // all annotations in fields
  walk_fields(m_scope,
              [&](DexField* field) { annos_in_aset(field->get_anno_set()); });

  AnnoKill::AnnoSet referenced_annos;

  // mark an annotation as "unremovable" if a field is typed with that
  // annotation
  walk_fields(m_scope, [&](DexField* field) {
    // don't look at fields defined on the annotation itself
    const auto field_cls_type = field->get_class();
    if (all_annos.count(field_cls_type) > 0) {
      return;
    }

    const auto field_cls = type_class(field_cls_type);
    if (field_cls != nullptr && is_annotation(field_cls)) {
      return;
    }

    auto ftype = field->get_type();
    if (all_annos.count(ftype) > 0) {
      TRACE(ANNO,
            3,
            "Field typed with an annotation type %s.%s:%s\n",
            SHOW(field->get_class()),
            SHOW(field->get_name()),
            SHOW(ftype));
      referenced_annos.insert(ftype);
    }
  });

  // mark an annotation as "unremovable" if a method signature contains a type
  // with that annotation
  walk_methods(m_scope, [&](DexMethod* meth) {
    // don't look at methods defined on the annotation itself
    const auto meth_cls_type = meth->get_class();
    if (all_annos.count(meth_cls_type) > 0) {
      return;
    }

    const auto meth_cls = type_class(meth_cls_type);
    if (meth_cls != nullptr && is_annotation(meth_cls)) {
      return;
    }

    const auto& has_anno = [&](DexType* type) {
      if (all_annos.count(type) > 0) {
        TRACE(ANNO,
              3,
              "Method contains annotation type in signature %s.%s:%s\n",
              SHOW(meth->get_class()),
              SHOW(meth->get_name()),
              SHOW(meth->get_proto()));
        referenced_annos.insert(type);
      }
    };

    const auto proto = meth->get_proto();
    has_anno(proto->get_rtype());
    for (const auto& arg : proto->get_args()->get_type_list()) {
      has_anno(arg);
    }
  });

  // mark an annotation as "unremovable" if any opcode references the annotation
  // type
  walk_opcodes(
      m_scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* meth, IRInstruction* insn) {
        // don't look at methods defined on the annotation itself
        const auto meth_cls_type = meth->get_class();
        if (all_annos.count(meth_cls_type) > 0) {
          return;
        }
        const auto meth_cls = type_class(meth_cls_type);
        if (meth_cls != nullptr && is_annotation(meth_cls)) {
          return;
        }

        if (insn->has_type()) {
          auto type = insn->get_type();
          if (all_annos.count(type) > 0) {
            referenced_annos.insert(type);
            TRACE(ANNO,
                  3,
                  "Annotation referenced in type opcode\n\t%s.%s:%s - %s\n",
                  SHOW(meth->get_class()),
                  SHOW(meth->get_name()),
                  SHOW(meth->get_proto()),
                  SHOW(insn));
          }
        } else if (insn->has_field()) {
          auto field = insn->get_field();
          auto fdef = resolve_field(field,
                                    is_sfield_op(insn->opcode())
                                        ? FieldSearch::Static
                                        : FieldSearch::Instance);
          if (fdef != nullptr) field = fdef;

          bool referenced = false;
          auto owner = field->get_class();
          if (all_annos.count(owner) > 0) {
            referenced = true;
            referenced_annos.insert(owner);
          }
          auto type = field->get_type();
          if (all_annos.count(type) > 0) {
            referenced = true;
            referenced_annos.insert(type);
          }
          if (referenced) {
            TRACE(ANNO,
                  3,
                  "Annotation referenced in field opcode\n\t%s.%s:%s - %s\n",
                  SHOW(meth->get_class()),
                  SHOW(meth->get_name()),
                  SHOW(meth->get_proto()),
                  SHOW(insn));
          }
        } else if (insn->has_method()) {
          auto method = insn->get_method();
          DexMethod* methdef = resolve_method(method, opcode_to_search(insn));
          if (methdef != nullptr) method = methdef;

          bool referenced = false;
          auto owner = method->get_class();
          if (all_annos.count(owner) > 0) {
            referenced = true;
            referenced_annos.insert(owner);
          }
          auto proto = method->get_proto();
          auto rtype = proto->get_rtype();
          if (all_annos.count(rtype) > 0) {
            referenced = true;
            referenced_annos.insert(rtype);
          }
          auto arg_list = proto->get_args();
          for (const auto& arg : arg_list->get_type_list()) {
            if (all_annos.count(arg) > 0) {
              referenced = true;
              referenced_annos.insert(arg);
            }
          }
          if (referenced) {
            TRACE(ANNO,
                  3,
                  "Annotation referenced in method opcode\n\t%s.%s:%s - %s\n",
                  SHOW(meth->get_class()),
                  SHOW(meth->get_name()),
                  SHOW(meth->get_proto()),
                  SHOW(insn));
          }
        }
      });
  return referenced_annos;
}

AnnoKill::AnnoSet AnnoKill::get_removable_annotation_instances() {
  // Determine which annotation classes are removable.
  std::unordered_set<DexType*> bannotations;
  for (auto clazz : m_scope) {
    if (!(clazz->get_access() & DexAccessFlags::ACC_ANNOTATION)) {
      continue;
    }

    auto aset = clazz->get_anno_set();
    if (!aset) {
      continue;
    }

    auto& annos = aset->get_annotations();
    for (auto anno : annos) {
      if (m_kill.count(anno->type())) {
        bannotations.insert(clazz->get_type());
        TRACE(ANNO,
              3,
              "removable annotation class %s\n",
              SHOW(clazz->get_type()));
      }
    }
  }
  return bannotations;
}

void AnnoKill::count_annotation(const DexAnnotation* da) {
  std::string annoName(da->type()->get_name()->c_str());
  if (da->system_visible()) {
    m_system_anno_map[annoName]++;
    m_stats.visibility_system_count++;
  } else if (da->runtime_visible()) {
    m_runtime_anno_map[annoName]++;
    m_stats.visibility_runtime_count++;
  } else if (da->build_visible()) {
    m_build_anno_map[annoName]++;
    m_stats.visibility_build_count++;
  }
}

void AnnoKill::cleanup_aset(DexAnnotationSet* aset,
                            const AnnoKill::AnnoSet& referenced_annos) {
  m_stats.annotations += aset->size();
  auto& annos = aset->get_annotations();
  auto fn = [&](DexAnnotation* da) {
    auto anno_type = da->type();
    count_annotation(da);

    if (referenced_annos.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Annotation type %s with type referenced in "
            "code, skipping...\n\tannotation: %s\n",
            SHOW(anno_type),
            SHOW(da));
      return false;
    }

    if (m_keep.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Blacklisted annotation type %s, "
            "skipping...\n\tannotation: %s\n",
            SHOW(anno_type),
            SHOW(da));
      return false;
    }

    if (m_kill.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Annotation instance (type: %s) marked for removal, "
            "annotation: %s\n",
            SHOW(anno_type),
            SHOW(da));
      m_stats.annotations_killed++;
      delete da;
      return true;
    }

    if (m_force_kill.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Annotation instance (type: %s) marked for forced removal, "
            "annotation: %s\n",
            SHOW(anno_type),
            SHOW(da));
      m_stats.annotations_killed++;
      delete da;
      return true;
    }

    if (!m_only_force_kill && !da->system_visible()) {
      TRACE(ANNO, 3, "Killing annotation instance %s\n", SHOW(da));
      m_stats.annotations_killed++;
      delete da;
      return true;
    }
    return false;
  };
  annos.erase(std::remove_if(annos.begin(), annos.end(), fn), annos.end());
}

bool AnnoKill::kill_annotations() {
  const auto& referenced_annos = get_referenced_annos();
  if (!m_only_force_kill) {
    m_kill = get_removable_annotation_instances();
  }

  for (auto clazz : m_scope) {
    DexAnnotationSet* aset = clazz->get_anno_set();
    if (!aset) {
      continue;
    }
    m_stats.class_asets++;
    cleanup_aset(aset, referenced_annos);
    if (aset->size() == 0) {
      TRACE(ANNO,
            3,
            "Clearing annotation for class %s\n",
            SHOW(clazz->get_type()));
      clazz->clear_annotations();
      m_stats.class_asets_cleared++;
    }
  }

  walk_methods(m_scope, [&](DexMethod* method) {
    // Method annotations
    auto method_aset = method->get_anno_set();
    if (method_aset) {
      m_stats.method_asets++;
      cleanup_aset(method_aset, referenced_annos);
      if (method_aset->size() == 0) {
        TRACE(ANNO,
              3,
              "Clearing annotations for method %s.%s:%s\n",
              SHOW(method->get_class()),
              SHOW(method->get_name()),
              SHOW(method->get_proto()));
        method->clear_annotations();
        m_stats.method_asets_cleared++;
      }
    }

    // Parameter annotations.
    auto param_annos = method->get_param_anno();
    if (param_annos) {
      m_stats.method_param_asets += param_annos->size();
      bool clear_pas = true;
      for (auto pa : *param_annos) {
        auto param_aset = pa.second;
        if (param_aset->size() == 0) {
          continue;
        }
        cleanup_aset(param_aset, referenced_annos);
        if (param_aset->size() == 0) {
          continue;
        }
        clear_pas = false;
      }
      if (clear_pas) {
        TRACE(ANNO,
              3,
              "Clearing parameter annotations for method parameters %s.%s:%s\n",
              SHOW(method->get_class()),
              SHOW(method->get_name()),
              SHOW(method->get_proto()));
        m_stats.method_param_asets_cleared += param_annos->size();
        for (auto pa : *param_annos) {
          delete pa.second;
        }
        param_annos->clear();
      }
    }
  });

  walk_fields(m_scope, [&](DexField* field) {
    DexAnnotationSet* aset = field->get_anno_set();
    if (!aset) {
      return;
    }
    m_stats.field_asets++;
    cleanup_aset(aset, referenced_annos);
    if (aset->size() == 0) {
      TRACE(ANNO,
            3,
            "Clearing annotations for field %s.%s:%s\n",
            SHOW(field->get_class()),
            SHOW(field->get_name()),
            SHOW(field->get_type()));
      field->clear_annotations();
      m_stats.field_asets_cleared++;
    }
  });

  bool classes_removed = false;
  // We're done removing annotation instances, go ahead and remove annotation
  // classes.
  m_scope.erase(std::remove_if(m_scope.begin(),
                               m_scope.end(),
                               [&](DexClass* cls) {
                                 if (!is_annotation(cls)) {
                                   return false;
                                 }
                                 auto type = cls->get_type();
                                 if (referenced_annos.count(type)) {
                                   return false;
                                 }
                                 if (m_keep.count(type)) {
                                   return false;
                                 }
                                 TRACE(ANNO,
                                       3,
                                       "Removing annotation type: %s\n",
                                       SHOW(type));
                                 classes_removed = true;
                                 return true;
                               }),
                m_scope.end());

  for (const auto& p : m_build_anno_map) {
    TRACE(ANNO, 3, "Build anno: %lu, %s\n", p.second, p.first.c_str());
  }

  for (const auto& p : m_runtime_anno_map) {
    TRACE(ANNO, 3, "Runtime anno: %lu, %s\n", p.second, p.first.c_str());
  }

  for (const auto& p : m_system_anno_map) {
    TRACE(ANNO, 3, "System anno: %lu, %s\n", p.second, p.first.c_str());
  }

  return classes_removed;
}

void AnnoKillPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {

  auto scope = build_class_scope(stores);

  AnnoKill ak(scope, only_force_kill(), m_keep_annos, m_kill_annos, m_force_kill_annos);
  bool classes_removed = ak.kill_annotations();

  if (classes_removed) {
    post_dexen_changes(scope, stores);
  }

  auto stats = ak.get_stats();

  TRACE(ANNO, 1, "AnnoKill report killed/total\n");
  TRACE(ANNO,
        1,
        "Annotations: %d/%d\n",
        stats.annotations_killed,
        stats.annotations);
  TRACE(ANNO,
        1,
        "Class Asets: %d/%d\n",
        stats.class_asets_cleared,
        stats.class_asets);
  TRACE(ANNO,
        1,
        "Method Asets: %d/%d\n",
        stats.method_asets_cleared,
        stats.method_asets);
  TRACE(ANNO,
        1,
        "MethodParam Asets: %d/%d\n",
        stats.method_param_asets_cleared,
        stats.method_param_asets);
  TRACE(ANNO,
        1,
        "Field Asets: %d/%d\n",
        stats.field_asets_cleared,
        stats.field_asets);

  TRACE(ANNO,
        3,
        "Total referenced Build Annos: %d\n",
        stats.visibility_build_count);
  TRACE(ANNO,
        3,
        "Total referenced Runtime Annos: %d\n",
        stats.visibility_runtime_count);
  TRACE(ANNO,
        3,
        "Total referenced System Annos: %d\n",
        stats.visibility_system_count);

  mgr.incr_metric(METRIC_ANNO_KILLED, stats.annotations_killed);
  mgr.incr_metric(METRIC_ANNO_TOTAL, stats.annotations);
  mgr.incr_metric(METRIC_CLASS_ASETS_CLEARED, stats.class_asets_cleared);
  mgr.incr_metric(METRIC_CLASS_ASETS_TOTAL, stats.class_asets);
  mgr.incr_metric(METRIC_METHOD_ASETS_CLEARED, stats.method_asets_cleared);
  mgr.incr_metric(METRIC_METHOD_ASETS_TOTAL, stats.method_asets);
  mgr.incr_metric(METRIC_METHODPARAM_ASETS_CLEARED,
                  stats.method_param_asets_cleared);
  mgr.incr_metric(METRIC_METHODPARAM_ASETS_TOTAL, stats.method_param_asets);
  mgr.incr_metric(METRIC_FIELD_ASETS_CLEARED, stats.field_asets_cleared);
  mgr.incr_metric(METRIC_FIELD_ASETS_TOTAL, stats.field_asets);
}

static AnnoKillPass s_pass;
