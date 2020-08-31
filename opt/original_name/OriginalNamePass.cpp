/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OriginalNamePass.h"
#include "ClassHierarchy.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "Show.h"

#define METRIC_MISSING_ORIGINAL_NAME_ROOT "num_missing_original_name_root"
#define METRIC_ORIGINAL_NAME_COUNT "num_original_name"

static OriginalNamePass s_pass;

static const char* redex_field_name = "__redex_internal_original_name";

void OriginalNamePass::build_hierarchies(
    PassManager& mgr,
    const ClassHierarchy& ch,
    Scope& scope,
    std::unordered_map<const DexType*, std::string>* hierarchies) {
  std::vector<DexClass*> base_classes;
  for (const auto& base : m_hierarchy_roots) {
    // skip comments
    if (base.c_str()[0] == '#') continue;
    auto base_type = DexType::get_type(base.c_str());
    auto base_class = base_type != nullptr ? type_class(base_type) : nullptr;
    if (base_class == nullptr) {
      TRACE(ORIGINALNAME, 2,
            "Can't find class for annotate_original_name rule %s",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_ORIGINAL_NAME_ROOT, 1);
    } else {
      base_classes.emplace_back(base_class);
    }
  }
  for (const auto& base_class : base_classes) {
    auto base_name = base_class->get_deobfuscated_name();
    hierarchies->emplace(base_class->get_type(), base_name);
    TypeSet children_and_implementors;
    get_all_children_or_implementors(ch, scope, base_class,
                                     children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      hierarchies->emplace(cls, base_name);
    }
  }
}

void OriginalNamePass::run_pass(DexStoresVector& stores,
                                ConfigFiles&,
                                PassManager& mgr) {
  auto scope = build_class_scope(stores);
  ClassHierarchy ch = build_type_hierarchy(scope);
  std::unordered_map<const DexType*, std::string> to_annotate;
  build_hierarchies(mgr, ch, scope, &to_annotate);
  DexString* field_name = DexString::make_string(redex_field_name);
  DexType* string_type = type::java_lang_String();
  for (const auto& it : to_annotate) {
    const DexType* cls_type = it.first;
    if (strncmp("LX/", cls_type->get_name()->c_str(), 3) != 0) {
      continue;
    }

    DexClass* cls = type_class(cls_type);
    auto external_name =
        java_names::internal_to_external(cls->get_deobfuscated_name());
    auto external_name_s = DexString::make_string(external_name.c_str());
    always_assert_log(DexField::get_field(cls_type, field_name, string_type) ==
                          nullptr,
                      "field %s already exists!",
                      redex_field_name);
    DexField* f =
        DexField::make_field(cls_type, field_name, string_type)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL,
                            new DexEncodedValueString(external_name_s));
    // These fields are accessed reflectively, so make sure we do not remove
    // them.
    f->rstate.set_root();
    insert_sorted(cls->get_sfields(), f, compare_dexfields);
    f->set_deobfuscated_name(show_deobfuscated(f));

    mgr.incr_metric(METRIC_ORIGINAL_NAME_COUNT, 1);
    mgr.incr_metric(std::string(METRIC_ORIGINAL_NAME_COUNT) + "::" + it.second,
                    1);
  }
}
