/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UsesNames.h"

#include "ClassHierarchy.h"
#include "DexUtil.h"
#include "Walkers.h"

/*
 * Find parameters annotated with @UsesNames and sets the name_used bit on
 * those parameter types and their subclasses.
 */
namespace {
class UsesNamesMarker {

 public:
  UsesNamesMarker(DexType* uses_names_anno, Scope& scope)
      : m_ch(build_type_hierarchy(scope)), uses_names_anno(uses_names_anno) {}

  static void mark_uses_names(DexClass* cls) {
    TRACE(USES_NAMES, 2, "Mark class and member: %s", show(cls).c_str());
    cls->rstate.set_name_used();
    for (DexMethod* dmethod : cls->get_dmethods()) {
      dmethod->rstate.set_name_used();
    }
    for (DexMethod* vmethod : cls->get_vmethods()) {
      vmethod->rstate.set_name_used();
    }
    for (DexField* sfield : cls->get_sfields()) {
      sfield->rstate.set_name_used();
    }
    for (DexField* ifield : cls->get_ifields()) {
      ifield->rstate.set_name_used();
    }
  }

  void mark_subclass_uses_names(const DexClass* cls) {
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
      mark_uses_names(c);
    }
  }

  bool match_uses_names_annotation(const DexAnnotationSet* annos) {
    if (!annos) return false;
    for (const auto& anno : annos->get_annotations()) {
      if (anno->type() == uses_names_anno) {
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
      if (match_uses_names_annotation(annos)) {
        TRACE(USES_NAMES, 2, "@UsesNames in method %s", vshow(meth).c_str());
        DexType* type = meth->get_proto()->get_args()->at(num);
        DexClass* cls = type_class(type);
        if (cls == nullptr) {
          TRACE(USES_NAMES, 2, "Class not found for: %s", show(type).c_str());
          continue;
        }
        mark_uses_names(cls);
        mark_subclass_uses_names(cls);
      }
    }
  }

 private:
  const ClassHierarchy m_ch;
  InterfaceMap m_interface_map;
  DexType* uses_names_anno;
};
} // namespace

void ProcessUsesNamesAnnoPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* conf */,
                                        PassManager& /* pm */) {
  Scope scope = build_class_scope(stores);
  UsesNamesMarker unm(m_uses_names_annotation, scope);
  walk::parallel::methods(
      scope, [&](DexMethod* meth) { unm.mark_uses_names_for_method(meth); });
}

static ProcessUsesNamesAnnoPass s_pass;
