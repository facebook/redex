/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UsesNames.h"

#include "ClassHierarchy.h"
#include "ConcurrentContainers.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

/*
 * Find parameters annotated with @UsesNames and sets the name_used bit on
 * those parameter types and their subclasses.
 */
namespace {
class UsesNamesMarker {

 public:
  UsesNamesMarker(DexType* uses_names_anno,
                  DexType* uses_names_trans_anno,
                  Scope& scope)
      : m_ch(build_type_hierarchy(scope)),
        uses_names_anno(uses_names_anno),
        uses_names_trans_anno(uses_names_trans_anno) {}

  void mark_uses_names(DexClass* cls) {
    TRACE(USES_NAMES, 3, "Mark class and member: %s", show(cls).c_str());
    cls->rstate.set_name_used();
    metrics.used_classes += 1;
    for (DexMethod* dmethod : cls->get_dmethods()) {
      dmethod->rstate.set_name_used();
      metrics.used_methods += 1;
    }
    for (DexMethod* vmethod : cls->get_vmethods()) {
      vmethod->rstate.set_name_used();
      metrics.used_methods += 1;
    }
    for (DexField* sfield : cls->get_sfields()) {
      sfield->rstate.set_name_used();
      metrics.used_fields += 1;
    }
    for (DexField* ifield : cls->get_ifields()) {
      ifield->rstate.set_name_used();
      metrics.used_fields += 1;
    }
  }

  /**
   * @param transitive Whether it is @UsesNamesTransitive
   * True value marks class of the field as used
   */
  void mark_subclass_uses_names(DexClass* cls, bool transitive) {
    TypeSet subclass_types;
    if (is_interface(cls)) {
      if (m_interface_map.empty()) {
        m_interface_map = build_interface_map(m_ch);
      }
      subclass_types = get_all_implementors(m_interface_map, cls->get_type());
    } else {
      subclass_types = get_all_children(m_ch, cls->get_type());
    }
    for (auto type : subclass_types) {
      DexClass* c = type_class(type);
      if (c == nullptr) {
        TRACE(USES_NAMES, 2, "Class not found: %s", show(type).c_str());
        return;
      }
      metrics.used_classes_by_subclass += 1;
      mark_class_uses_names_recursive(c, transitive);
    }
  }

  void mark_field_class_uses_names(DexClass* cls) {
    for (const auto& field : cls->get_ifields()) {
      DexClass* c = type_class(field->get_type());
      if (c == nullptr) {
        TRACE(USES_NAMES,
              2,
              "Class not found for: %s",
              show(field->get_type()).c_str());
        continue;
      }
      metrics.used_classes_by_field += 1;
      mark_class_uses_names_recursive(c, true);
    }
  }

  /**
   * @param transitive Whether it is @UsesNamesTransitive
   * True value marks class of the field as used
   */
  void mark_class_uses_names_recursive(DexClass* cls, bool transitive) {
    // stop if already marked
    if (cls->rstate.name_used()) {
      return;
    }

    // do not mark external class or its field class or its subclasses
    if (cls->is_external()) {
      return;
    }

    mark_uses_names(cls);
    if (transitive) {
      mark_field_class_uses_names(cls);
    }
    mark_subclass_uses_names(cls, transitive);
  }

  static bool match_uses_names_annotation(const DexAnnotationSet* annos,
                                          DexType* anno_type) {
    if (!annos) return false;
    for (const auto& anno : annos->get_annotations()) {
      if (anno->type() == anno_type) {
        return true;
      }
    }
    return false;
  }

  void mark_uses_names_for_method(const DexMethod* meth) {
    if (!meth->get_param_anno()) {
      return;
    }
    for (auto const& pair : *meth->get_param_anno()) {
      int num = pair.first;
      const DexAnnotationSet* annos = pair.second;
      if (!annos) {
        continue;
      }

      bool has_uses_names = match_uses_names_annotation(annos, uses_names_anno);
      bool has_uses_name_trans =
          match_uses_names_annotation(annos, uses_names_trans_anno);

      if (has_uses_names || has_uses_name_trans) {
        DexType* matched_anno;
        if (has_uses_names) {
          matched_anno = uses_names_anno;
          metrics.uses_names_anno += 1;
        } else {
          matched_anno = uses_names_trans_anno;
          metrics.uses_names_trans_anno += 1;
        }
        TRACE(USES_NAMES,
              2,
              "%s annotation found in method %s",
              show(matched_anno).c_str(),
              vshow(meth).c_str());
        DexType* type = meth->get_proto()->get_args()->at(num);
        DexClass* cls = type_class(type);
        if (cls == nullptr) {
          TRACE(USES_NAMES, 2, "Class not found for: %s", show(type).c_str());
          continue;
        }
        if (cls->is_external()) {
          fprintf(stderr,
                  "Should not use UsesNames Annotation on external class %s",
                  show(cls).c_str());
          continue;
        }
        mark_class_uses_names_recursive(cls, has_uses_name_trans);
      }
    }
  }

  ProcessUsesNamesAnnoPass::Metrics metrics;

 private:
  const ClassHierarchy m_ch;
  InterfaceMap m_interface_map;
  DexType* uses_names_anno;
  DexType* uses_names_trans_anno;
};
} // namespace

void ProcessUsesNamesAnnoPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* conf */,
                                        PassManager& pm) {
  Scope scope = build_class_scope(stores);
  UsesNamesMarker unm(
      m_uses_names_annotation, m_uses_names_trans_annotation, scope);
  walk::parallel::methods(
      scope, [&](DexMethod* meth) { unm.mark_uses_names_for_method(meth); });
  pm.incr_metric("Total class used", unm.metrics.used_classes);
  pm.incr_metric("Total class used by transitive to subclass",
                 unm.metrics.used_classes_by_subclass);
  pm.incr_metric("Total class used by transitive to field",
                 unm.metrics.used_classes_by_field);
  pm.incr_metric("Total fields used", unm.metrics.used_fields);
  pm.incr_metric("Total methods used", unm.metrics.used_methods);
  pm.incr_metric("@UsesNames annotation", unm.metrics.uses_names_anno);
  pm.incr_metric("@UsesNamesTransitive annotation",
                 unm.metrics.uses_names_trans_anno);
}

static ProcessUsesNamesAnnoPass s_pass;
