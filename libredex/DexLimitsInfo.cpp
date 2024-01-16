/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexLimitsInfo.h"
#include "Show.h"
#include "Trace.h"

DexLimitsInfo::DexLimitsInfo(
    init_classes::InitClassesWithSideEffects* init_classes_with_side_effects,
    const DexClasses& dex) {
  m_init_classes_with_side_effects = init_classes_with_side_effects;
  for (const auto cls : dex) {
    always_assert_log(update_refs_by_adding_class(cls),
                      "Dex limitation breaks");
  }
}

bool DexLimitsInfo::update_refs_by_adding_class(DexClass* cls) {
  MethodRefs method_refs;
  FieldRefs field_refs;
  TypeRefs type_refs;
  TypeRefs init_refs;
  std::vector<DexType*> itrefs;
  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;

  if (m_init_classes_with_side_effects) {
    cls->gather_init_classes(itrefs);
    init_refs.insert(itrefs.begin(), itrefs.end());
  }
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(type_refs);

  m_dex.resolve_init_classes(m_init_classes_with_side_effects, field_refs,
                             type_refs, init_refs, &pending_init_class_fields,
                             &pending_init_class_types);

  return m_dex.add_class_if_fits(method_refs,
                                 field_refs,
                                 type_refs,
                                 pending_init_class_fields,
                                 pending_init_class_types,
                                 m_linear_alloc_limit,
                                 m_field_limit,
                                 m_meth_limit,
                                 m_type_limit,
                                 cls);
}

void DexLimitsInfo::update_refs_by_always_adding_class(DexClass* cls) {
  MethodRefs method_refs;
  FieldRefs field_refs;
  TypeRefs type_refs;
  TypeRefs init_refs;
  std::vector<DexType*> itrefs;
  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;

  if (m_init_classes_with_side_effects) {
    cls->gather_init_classes(itrefs);
    init_refs.insert(itrefs.begin(), itrefs.end());
  }
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(type_refs);

  m_dex.resolve_init_classes(m_init_classes_with_side_effects, field_refs,
                             type_refs, init_refs, &pending_init_class_fields,
                             &pending_init_class_types);

  return m_dex.add_class_no_checks(method_refs,
                                   field_refs,
                                   type_refs,
                                   pending_init_class_fields,
                                   pending_init_class_types,
                                   /*laclazz=*/0,
                                   cls);
}

void DexLimitsInfo::update_refs_by_erasing_class(DexClass* cls) {
  MethodRefs method_refs;
  FieldRefs field_refs;
  TypeRefs type_refs;
  TypeRefs init_refs;
  std::vector<DexType*> itrefs;
  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;

  if (m_init_classes_with_side_effects) {
    cls->gather_init_classes(itrefs);
    init_refs.insert(itrefs.begin(), itrefs.end());
  }
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(type_refs);

  m_dex.resolve_init_classes(m_init_classes_with_side_effects, field_refs,
                             type_refs, init_refs, &pending_init_class_fields,
                             &pending_init_class_types);
  auto laclazz = estimate_linear_alloc(cls);

  m_dex.remove_class(m_init_classes_with_side_effects,
                     method_refs,
                     field_refs,
                     type_refs,
                     pending_init_class_fields,
                     pending_init_class_types,
                     laclazz,
                     cls);
}
