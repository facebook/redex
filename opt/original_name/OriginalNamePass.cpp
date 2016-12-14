/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexUtil.h"
#include "OriginalNamePass.h"

#define METRIC_MISSING_ORIGINAL_NAME_ROOT "num_missing_original_name_root"
#define METRIC_SET_CLASS_DATA "num_set_class_data"
#define METRIC_ORIGINAL_NAME_COUNT "num_original_name"

static OriginalNamePass s_pass;

static const char* redex_field_name = "__redex_internal_original_name";

void OriginalNamePass::build_hierarchies(
    PassManager& mgr,
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
            "Can't find class for annotate_original_name rule %s\n",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_ORIGINAL_NAME_ROOT, 1);
    } else {
      base_classes.emplace_back(base_class);
    }
  }
  for (const auto& base_class : base_classes) {
    auto base_name = base_class->get_deobfuscated_name();
    hierarchies->emplace(base_class->get_type(), base_name);
    std::unordered_set<const DexType*> children_and_implementors;
    get_all_children_and_implementors(
        scope, base_class, &children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      hierarchies->emplace(cls, base_name);
    }
  }
}

void OriginalNamePass::run_pass(DexStoresVector& stores,
                                ConfigFiles&,
                                PassManager& mgr) {
  auto scope = build_class_scope(stores);
  std::unordered_map<const DexType*, std::string> to_annotate;
  build_hierarchies(mgr, scope, &to_annotate);
  DexString* field_name = DexString::make_string(redex_field_name);
  DexType* string_type = get_string_type();
  for (auto it : to_annotate) {
    const DexType* cls_type = it.first;
    if (strncmp("LX/", cls_type->get_name()->c_str(), 2) != 0) {
      continue;
    }

    DexClass* cls = type_class(cls_type);
    if (!cls->has_class_data()) {
      cls->set_class_data(true);
      mgr.incr_metric(METRIC_SET_CLASS_DATA, 1);
    }

    auto external_name =
        JavaNameUtil::internal_to_external(cls->get_deobfuscated_name());
    auto external_name_s = DexString::make_string(external_name.c_str());
    always_assert_log(DexField::get_field(cls_type, field_name, string_type) ==
                          nullptr,
                      "field %s already exists!",
                      redex_field_name);
    DexField* f = DexField::make_field(cls_type, field_name, string_type);
    f->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL,
                     new DexEncodedValueString(external_name_s));
    insert_sorted(cls->get_sfields(), f, compare_dexfields);

    mgr.incr_metric(METRIC_ORIGINAL_NAME_COUNT, 1);
    mgr.incr_metric(std::string(METRIC_ORIGINAL_NAME_COUNT) + "::" + it.second,
                    1);
  }
}
