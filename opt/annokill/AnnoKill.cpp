/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnnoKill.h"

#include "ClassHierarchy.h"
#include "Debug.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
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
constexpr const char* METRIC_SIGNATURES_KILLED = "num_signatures_killed";

AnnoKill::AnnoKill(
    Scope& scope,
    bool only_force_kill,
    bool kill_bad_signatures,
    const AnnoNames& keep,
    const AnnoNames& kill,
    const AnnoNames& force_kill,
    const std::unordered_map<std::string, std::vector<std::string>>&
        class_hierarchy_keep_annos,
    const std::unordered_map<std::string, std::vector<std::string>>&
        annotated_keep_annos)
    : m_scope(scope),
      m_only_force_kill(only_force_kill),
      m_kill_bad_signatures(kill_bad_signatures) {
  TRACE(ANNO,
        2,
        "only_force_kill=%u kill_bad_signatures=%d",
        m_only_force_kill,
        kill_bad_signatures);
  // Load annotations that should not be deleted.
  TRACE(ANNO, 2, "Keep annotations count %zu", keep.size());
  for (const auto& anno_name : keep) {
    auto anno_type = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Keep annotation type string %s", anno_name.c_str());
    if (anno_type) {
      TRACE(ANNO, 2, "Keep annotation type %s", SHOW(anno_type));
      m_keep.insert(anno_type);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s", anno_name.c_str());
    }
  }

  // Load annotations we know and want dead.
  for (auto const& anno_name : kill) {
    DexType* anno = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Kill annotation type string %s", anno_name.c_str());
    if (anno) {
      TRACE(ANNO, 2, "Kill anno: %s", SHOW(anno));
      m_kill.insert(anno);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s", anno_name.c_str());
    }
  }

  // Load annotations we know and want dead.
  for (auto const& anno_name : force_kill) {
    DexType* anno = DexType::get_type(anno_name.c_str());
    TRACE(ANNO, 2, "Force kill annotation type string %s", anno_name.c_str());
    if (anno) {
      TRACE(ANNO, 2, "Force kill anno: %s", SHOW(anno));
      m_force_kill.insert(anno);
    } else {
      TRACE(ANNO, 2, "Cannot find annotation type %s", anno_name.c_str());
    }
  }

  // Populate class hierarchy keep map
  auto ch = build_type_hierarchy(m_scope);
  for (const auto& it : class_hierarchy_keep_annos) {
    auto* type = DexType::get_type(it.first.c_str());
    auto* type_cls = type ? type_class(type) : nullptr;
    if (type_cls == nullptr) {
      continue;
    }

    TypeSet type_refs;
    get_all_children_or_implementors(ch, m_scope, type_cls, type_refs);
    for (auto& anno : it.second) {
      auto* anno_type = DexType::get_type(anno.c_str());
      for (auto type_ref : type_refs) {
        m_anno_class_hierarchy_keep[type_ref].insert(anno_type);
      }
    }
  }
  for (const auto& it : m_anno_class_hierarchy_keep) {
    for (auto type : it.second) {
      TRACE(ANNO,
            4,
            "anno_class_hier_keep: %s -> %s",
            it.first->get_name()->c_str(),
            type->get_name()->c_str());
    }
  }
  // Populate anno keep map
  for (const auto& it : annotated_keep_annos) {
    auto* type = DexType::get_type(it.first.c_str());
    for (auto& anno : it.second) {
      auto* anno_type = DexType::get_type(anno.c_str());
      m_annotated_keep_annos[type].insert(anno_type);
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
  walk::methods(m_scope, [&](DexMethod* method) {
    annos_in_aset(method->get_anno_set());
    auto param_annos = method->get_param_anno();
    if (!param_annos) {
      return;
    }
    for (auto& pa : *param_annos) {
      annos_in_aset(pa.second.get());
    }
  });
  // all annotations in fields
  walk::fields(m_scope,
               [&](DexField* field) { annos_in_aset(field->get_anno_set()); });

  AnnoKill::AnnoSet referenced_annos;

  // mark an annotation as "unremovable" if a field is typed with that
  // annotation
  walk::fields(m_scope, [&](DexField* field) {
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
            "Field typed with an annotation type %s.%s:%s",
            SHOW(field->get_class()),
            SHOW(field->get_name()),
            SHOW(ftype));
      referenced_annos.insert(ftype);
    }
  });

  // mark an annotation as "unremovable" if a method signature contains a type
  // with that annotation
  walk::methods(m_scope, [&](DexMethod* meth) {
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
              "Method contains annotation type in signature %s.%s:%s",
              SHOW(meth->get_class()),
              SHOW(meth->get_name()),
              SHOW(meth->get_proto()));
        referenced_annos.insert(type);
      }
    };

    const auto proto = meth->get_proto();
    has_anno(proto->get_rtype());
    for (const auto& arg : *proto->get_args()) {
      has_anno(arg);
    }
  });

  ConcurrentSet<DexType*> concurrent_referenced_annos;
  auto add_concurrent_referenced_anno = [&](DexType* t) {
    if (!referenced_annos.count(t)) {
      concurrent_referenced_annos.insert(t);
    }
  };
  // mark an annotation as "unremovable" if any opcode references the annotation
  // type
  walk::parallel::opcodes(
      m_scope,
      [](DexMethod*) { return true; },
      [&add_concurrent_referenced_anno, &all_annos](DexMethod* meth,
                                                    IRInstruction* insn) {
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
            add_concurrent_referenced_anno(type);
            TRACE(ANNO,
                  3,
                  "Annotation referenced in type opcode\n\t%s.%s:%s - %s",
                  SHOW(meth->get_class()),
                  SHOW(meth->get_name()),
                  SHOW(meth->get_proto()),
                  SHOW(insn));
          }
        } else if (insn->has_field()) {
          auto field = insn->get_field();
          auto fdef = resolve_field(field,
                                    opcode::is_an_sfield_op(insn->opcode())
                                        ? FieldSearch::Static
                                        : FieldSearch::Instance);
          if (fdef != nullptr) field = fdef;

          bool referenced = false;
          auto owner = field->get_class();
          if (all_annos.count(owner) > 0) {
            referenced = true;
            add_concurrent_referenced_anno(owner);
          }
          auto type = field->get_type();
          if (all_annos.count(type) > 0) {
            referenced = true;
            add_concurrent_referenced_anno(type);
          }
          if (referenced) {
            TRACE(ANNO,
                  3,
                  "Annotation referenced in field opcode\n\t%s.%s:%s - %s",
                  SHOW(meth->get_class()),
                  SHOW(meth->get_name()),
                  SHOW(meth->get_proto()),
                  SHOW(insn));
          }
        } else if (insn->has_method()) {
          auto method = insn->get_method();
          DexMethod* methdef =
              resolve_method(method, opcode_to_search(insn), meth);
          if (methdef != nullptr) method = methdef;

          bool referenced = false;
          auto owner = method->get_class();
          if (all_annos.count(owner) > 0) {
            referenced = true;
            add_concurrent_referenced_anno(owner);
          }
          auto proto = method->get_proto();
          auto rtype = proto->get_rtype();
          if (all_annos.count(rtype) > 0) {
            referenced = true;
            add_concurrent_referenced_anno(rtype);
          }
          auto arg_list = proto->get_args();
          for (const auto& arg : *arg_list) {
            if (all_annos.count(arg) > 0) {
              referenced = true;
              add_concurrent_referenced_anno(arg);
            }
          }
          if (referenced) {
            TRACE(ANNO,
                  3,
                  "Annotation referenced in method opcode\n\t%s.%s:%s - %s",
                  SHOW(meth->get_class()),
                  SHOW(meth->get_name()),
                  SHOW(meth->get_proto()),
                  SHOW(insn));
          }
        }
      });
  referenced_annos.insert(concurrent_referenced_annos.begin(),
                          concurrent_referenced_annos.end());
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
        TRACE(
            ANNO, 3, "removable annotation class %s", SHOW(clazz->get_type()));
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

void AnnoKill::cleanup_aset(
    DexAnnotationSet* aset,
    const AnnoKill::AnnoSet& referenced_annos,
    const std::unordered_set<const DexType*>& keep_annos) {
  m_stats.annotations += aset->size();
  auto& annos = aset->get_annotations();
  auto fn = [&](DexAnnotation* da) {
    auto anno_type = da->type();
    count_annotation(da);

    if (referenced_annos.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Annotation type %s with type referenced in "
            "code, skipping...\n\tannotation: %s",
            SHOW(anno_type),
            SHOW(da));
      return false;
    }

    if (keep_annos.count(anno_type) > 0) {
      TRACE(ANNO, 4, "Prohibited from removing annotation %s", SHOW(da));
      return false;
    }

    if (m_keep.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Exclude annotation type %s, "
            "skipping...\n\tannotation: %s",
            SHOW(anno_type),
            SHOW(da));
      return false;
    }

    if (m_kill.count(anno_type) > 0) {
      TRACE(ANNO,
            3,
            "Annotation instance (type: %s) marked for removal, "
            "annotation: %s",
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
            "annotation: %s",
            SHOW(anno_type),
            SHOW(da));
      m_stats.annotations_killed++;
      delete da;
      return true;
    }

    if (!m_only_force_kill && !da->system_visible()) {
      TRACE(ANNO, 3, "Killing annotation instance %s", SHOW(da));
      m_stats.annotations_killed++;
      delete da;
      return true;
    }

    if (anno_type == DexType::get_type("Ldalvik/annotation/Signature;")) {
      if (should_kill_bad_signature(da)) {
        m_stats.signatures_killed++;
        delete da;
        return true;
      }
    }

    return false;
  };
  annos.erase(std::remove_if(annos.begin(), annos.end(), fn), annos.end());
}

bool AnnoKill::should_kill_bad_signature(DexAnnotation* da) {
  if (!m_kill_bad_signatures) return false;
  TRACE(ANNO, 3, "Examining @Signature instance %s", SHOW(da));
  auto elems = da->anno_elems();
  for (auto elem : elems) {
    auto ev = elem.encoded_value;
    if (ev->evtype() != DEVT_ARRAY) continue;
    auto arrayev = static_cast<DexEncodedValueArray*>(ev);
    auto const& evs = arrayev->evalues();
    for (auto strev : *evs) {
      if (strev->evtype() != DEVT_STRING) continue;
      const auto& sigstr =
          static_cast<DexEncodedValueString*>(strev)->string()->str();
      always_assert(sigstr.length() > 0);
      const auto* sigcstr = sigstr.c_str();
      // @Signature grammar is non-trivial[1], nevermind the fact that
      // Signatures are broken up into arbitrary arrays of strings concatenated
      // at runtime. It seems like types are reliably never broken apart, so we
      // can usually find an entire type name in each DexEncodedValueString.
      //
      // We also crudely approximate that something looks like a typename in the
      // first place since there's a lot of mark up in the @Signature grammar,
      // e.g. formal type parameter names. We look for things that look like
      // "L*/*", don't include ":" (formal type parameter separator), and may or
      // may not end with a semicolon or angle bracket.
      //
      // I'm working on a C++ port of the AOSP generic signature parser so we
      // can make this more robust in the future.
      //
      // [1] androidxref.com/8.0.0_r4/xref/libcore/luni/src/main/java/libcore/
      //     reflect/GenericSignatureParser.java
      if (sigstr[0] == 'L' && strchr(sigcstr, '/') && !strchr(sigcstr, ':')) {
        auto* sigtype = DexType::get_type(sigstr.c_str());
        if (!sigtype) {
          // Try with semicolon.
          sigtype = DexType::get_type(sigstr + ';');
        }
        if (!sigtype && sigstr.back() == '<') {
          // Try replacing angle bracket with semicolon
          // d8 often encodes signature annotations this way
          std::string copy = sigstr;
          copy.pop_back();
          copy.push_back(';');
          sigtype = DexType::get_type(copy);
        }
        if (sigtype) {
          auto* sigcls = type_class(sigtype);
          if (!sigcls) {
            sigtype = nullptr;
          } else if (!sigcls->is_external()) {
            bool found = false;
            for (auto cls : m_scope) {
              if (cls == sigcls) {
                // Valid class, we're good, go to element in array
                found = true;
                continue;
              }
            }
            // Could not find the (non-external) class in Scope, so set signal
            // to kill
            if (!found) {
              sigtype = nullptr;
            }
          }
        }
        if (!sigtype) {
          TRACE(ANNO, 3, "Killing bad @Signature: %s", sigcstr);
          return true;
        }
      }
    }
  }
  return false;
}

std::unordered_set<const DexType*> AnnoKill::build_anno_keep(
    DexAnnotationSet* aset) {
  std::unordered_set<const DexType*> keep_list;
  for (const auto& anno : aset->get_annotations()) {
    auto& keeps = m_annotated_keep_annos[anno->type()];
    keep_list.insert(keeps.begin(), keeps.end());
  }
  return keep_list;
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
    auto keep_list = build_anno_keep(aset);
    auto& class_hier_keep_list = m_anno_class_hierarchy_keep[clazz->get_type()];
    keep_list.insert(class_hier_keep_list.begin(), class_hier_keep_list.end());

    m_stats.class_asets++;
    cleanup_aset(aset, referenced_annos, keep_list);
    if (aset->size() == 0) {
      TRACE(
          ANNO, 3, "Clearing annotation for class %s", SHOW(clazz->get_type()));
      clazz->clear_annotations();
      m_stats.class_asets_cleared++;
    }
  }

  walk::methods(m_scope, [&](DexMethod* method) {
    // Method annotations
    auto method_aset = method->get_anno_set();
    if (method_aset) {
      m_stats.method_asets++;
      auto keep_list = build_anno_keep(method_aset);
      cleanup_aset(method_aset, referenced_annos, keep_list);
      if (method_aset->size() == 0) {
        TRACE(ANNO,
              3,
              "Clearing annotations for method %s.%s:%s",
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
      for (auto& pa : *param_annos) {
        auto& param_aset = pa.second;
        if (param_aset->size() == 0) {
          continue;
        }
        auto keep_list = build_anno_keep(param_aset.get());
        cleanup_aset(param_aset.get(), referenced_annos, keep_list);
        if (param_aset->size() == 0) {
          continue;
        }
        clear_pas = false;
      }
      if (clear_pas) {
        TRACE(ANNO,
              3,
              "Clearing parameter annotations for method parameters %s.%s:%s",
              SHOW(method->get_class()),
              SHOW(method->get_name()),
              SHOW(method->get_proto()));
        m_stats.method_param_asets_cleared += param_annos->size();
        param_annos->clear();
      }
    }
  });

  walk::fields(m_scope, [&](DexField* field) {
    DexAnnotationSet* aset = field->get_anno_set();
    if (!aset) {
      return;
    }
    m_stats.field_asets++;
    auto keep_list = build_anno_keep(aset);
    cleanup_aset(aset, referenced_annos, keep_list);
    if (aset->size() == 0) {
      TRACE(ANNO,
            3,
            "Clearing annotations for field %s.%s:%s",
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
  m_scope.erase(
      std::remove_if(m_scope.begin(),
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
                       TRACE(
                           ANNO, 3, "Removing annotation type: %s", SHOW(type));
                       classes_removed = true;
                       return true;
                     }),
      m_scope.end());

  for (const auto& p : m_build_anno_map) {
    TRACE(ANNO, 3, "Build anno: %lu, %s", p.second, p.first.c_str());
  }

  for (const auto& p : m_runtime_anno_map) {
    TRACE(ANNO, 3, "Runtime anno: %lu, %s", p.second, p.first.c_str());
  }

  for (const auto& p : m_system_anno_map) {
    TRACE(ANNO, 3, "System anno: %lu, %s", p.second, p.first.c_str());
  }

  return classes_removed;
}

void AnnoKillPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {

  auto scope = build_class_scope(stores);

  AnnoKill ak(scope,
              only_force_kill(),
              m_kill_bad_signatures,
              m_keep_annos,
              m_kill_annos,
              m_force_kill_annos,
              m_class_hierarchy_keep_annos,
              m_annotated_keep_annos);
  bool classes_removed = ak.kill_annotations();

  if (classes_removed) {
    post_dexen_changes(scope, stores);
  }

  auto stats = ak.get_stats();

  TRACE(ANNO, 1, "AnnoKill report killed/total");
  TRACE(ANNO,
        1,
        "Annotations: %zu/%zu",
        stats.annotations_killed,
        stats.annotations);
  TRACE(ANNO,
        1,
        "Class Asets: %zu/%zu",
        stats.class_asets_cleared,
        stats.class_asets);
  TRACE(ANNO,
        1,
        "Method Asets: %zu/%zu",
        stats.method_asets_cleared,
        stats.method_asets);
  TRACE(ANNO,
        1,
        "MethodParam Asets: %zu/%zu",
        stats.method_param_asets_cleared,
        stats.method_param_asets);
  TRACE(ANNO,
        1,
        "Field Asets: %zu/%zu",
        stats.field_asets_cleared,
        stats.field_asets);

  TRACE(ANNO,
        3,
        "Total referenced Build Annos: %zu",
        stats.visibility_build_count);
  TRACE(ANNO,
        3,
        "Total referenced Runtime Annos: %zu",
        stats.visibility_runtime_count);
  TRACE(ANNO,
        3,
        "Total referenced System Annos: %zu",
        stats.visibility_system_count);
  TRACE(ANNO, 1, "@Signatures Killed: %zu", stats.signatures_killed);

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
  mgr.incr_metric(METRIC_SIGNATURES_KILLED, stats.signatures_killed);
}

static AnnoKillPass s_pass;
