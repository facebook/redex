/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassMerging.h"

#include "ClassAssemblingUtils.h"
#include "ModelMerger.h"
#include "NormalizeConstructor.h"

std::string ModelMerger::s_mapping_file;
std::string Model::s_outdir;

namespace class_merging {

const std::string TE_MAPPING_FILE_NAME = "redex-type-erasure-mappings.txt";

bool s_is_initialized = false;

/*
 * Initialize a number of static states for the output mapping file and the
 * interdex group mapping.
 */
void set_up(ConfigFiles& conf) {
  if (s_is_initialized) {
    // Already initialized.
    return;
  }
  ModelMerger::s_mapping_file = conf.metafile(TE_MAPPING_FILE_NAME);
  Model::s_outdir = conf.get_outdir();
  Model::build_interdex_groups(&conf);
  s_is_initialized = true;
}

void merge_model(Scope& scope,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec) {
  always_assert(s_is_initialized);
  handle_interface_as_root(spec, scope, stores);
  TRACE(TERA, 2, "[TERA] merging %s model", spec.name.c_str());
  Timer t("erase_model");
  for (const auto root : spec.roots) {
    always_assert(!is_interface(type_class(root)));
  }
  TypeSystem type_system(scope);
  auto model = Model::build_model(scope, stores, spec, type_system);
  model.update_redex_stats(mgr);

  ModelMerger mm;
  auto merger_classes = mm.merge_model(scope, stores, model);
  auto num_dedupped = method_dedup::dedup_constructors(merger_classes, scope);
  mm.increase_ctor_dedupped_stats(num_dedupped);
  mm.update_redex_stats(spec.class_name_prefix, mgr);
}

} // namespace class_merging
