/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeErasurePass.h"

#include "ClassAssemblingUtils.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "Model.h"
#include "ModelMerger.h"
#include "PluginRegistry.h"
#include "TypeErasureInterDexPlugin.h"

namespace {

DexType* get_type(const std::string& type_s) {
  auto type = DexType::get_type(type_s.c_str());
  if (type == nullptr) {
    fprintf(stderr,
            "[TERA] Warning: No type found for target type %s\n",
            type_s.c_str());
  }
  return type;
}

std::vector<DexType*> get_types(const std::vector<std::string>& target_types) {
  std::vector<DexType*> types;
  for (const auto& type_s : target_types) {
    auto target_type = get_type(type_s);
    if (target_type == nullptr) continue;
    types.push_back(target_type);
  }
  return types;
}

/**
 * Verify model specs are consistent
 */
bool verify_model_spec(const ModelSpec& model_spec) {
  if (model_spec.name.empty()) {
    fprintf(stderr, "[TERA] Wrong specification: model must have \"name\"\n");
    return false;
  }

  if (model_spec.class_name_prefix.empty()) {
    fprintf(stderr,
            "[TERA] Wrong specification: model %s must have "
            "\"class_name_prefix\"\n",
            model_spec.name.c_str());
    return false;
  }

  if (model_spec.root == nullptr) {
    fprintf(stderr,
            "[TERA] Wrong specification: model %s must have \"roots\"\n",
            model_spec.name.c_str());
    return false;
  }

  return true;
}

} // namespace

void TypeErasurePass::configure_pass(const JsonWrapper& jw) {
  jw.get("merged_type_mappings", "", m_merged_type_mapping_file);
  bool devirtualize_non_virtuals;
  jw.get("devirtualize", false, devirtualize_non_virtuals);
  bool process_method_meta;
  jw.get("process_method_meta", false, process_method_meta);
  int64_t max_num_dispatch_target;
  jw.get("max_num_dispatch_target", 0, max_num_dispatch_target);
  if (max_num_dispatch_target > 0) {
    m_max_num_dispatch_target =
        boost::optional<size_t>(static_cast<size_t>(max_num_dispatch_target));
  }

  // load model specifications
  Json::Value models;
  jw.get("models", Json::Value(), models);
  if (models.isNull()) return;
  if (!models.isArray()) {
    fprintf(stderr, "[TERA] Wrong specification: \"models\" is not an array\n");
    return;
  }

  const auto& load_types = [&](std::vector<std::string> type_names,
                               std::unordered_set<DexType*>& types) {
    std::vector<DexType*> ts = get_types(type_names);
    for (const auto& t : ts) {
      const auto& cls = type_class(t);
      if (cls == nullptr) {
        fprintf(stderr, "[TERA] Missing definition for type\n%s\n", SHOW(t));
        types.clear();
        return;
      }
      types.insert(t);
    }
  };

  // load each model spec for erasure
  for (auto it = models.begin(); it != models.end(); ++it) {
    const auto& value = *it;
    if (!value.isObject()) {
      fprintf(stderr,
              "[TERA] Wrong specification: model in array not an object\n");
      m_model_specs.clear();
      return;
    }
    JsonWrapper model_spec = JsonWrapper(value);
    ModelSpec model;
    model_spec.get("enabled", true, model.enabled);
    model_spec.get("needs_type_tag", true, model.needs_type_tag);
    model_spec.get("has_type_tag", false, model.has_type_tag);
    size_t min_count;
    model_spec.get("min_count", 1, min_count);
    model.min_count = min_count > 0 ? min_count : 0;
    model_spec.get("name", "", model.name);
    std::string root_name;
    model_spec.get("root", "", root_name);
    model.root = get_type(root_name);
    std::vector<std::string> excl_names;
    model_spec.get("exclude", {}, excl_names);
    load_types(excl_names, model.exclude_types);
    model_spec.get("class_name_prefix", "", model.class_name_prefix);
    Json::Value generated;
    model_spec.get("generated", Json::Value(), generated);
    if (!generated.isNull()) {
      if (!generated.isObject()) {
        fprintf(stderr,
                "[TERA] Wrong specification: model in array not an object\n");
        m_model_specs.clear();
        return;
      }
      JsonWrapper gen_spec = JsonWrapper(generated);
      std::vector<std::string> gen_names;
      gen_spec.get("other_roots", {}, gen_names);
      load_types(gen_names, model.gen_types);
    }
    model_spec.get("include_primary_dex", false, model.include_primary_dex);
    model_spec.get("dex_sharding", false, model.dex_sharding);

    model_spec.get("merge_per_interdex_set", false,
                   model.merge_per_interdex_set);
    always_assert_log(!model.merge_per_interdex_set || model.needs_type_tag,
                      "Cannot group when type tag is not needed.");
    always_assert_log(!model.dex_sharding || !model.merge_per_interdex_set,
                      "Cannot have both dex sharding and group sharding "
                      "enabled!");

    size_t max_count;
    model_spec.get("max_count", 0, max_count);
    Json::Value approximate_shaping;
    model_spec.get("approximate_shape_merging", Json::Value(),
                   model.approximate_shape_merging);
    model_spec.get("merge_types_with_static_fields", false,
                   model.merge_types_with_static_fields);
    model_spec.get("keep_debug_info", false, model.keep_debug_info);
    if (max_count > 0) {
      model.max_count = boost::optional<size_t>(max_count);
    }
    model.devirtualize_non_virtuals = devirtualize_non_virtuals;
    model.process_method_meta = process_method_meta;
    if (!verify_model_spec(model)) {
      continue;
    }

    if (model.dex_sharding) {
      if (!model.enabled) {
        TRACE(TERA, 3, "Per dex Type Erased model not enabled. Skipping %s\n",
              model.name.c_str());
      } else {
        m_dex_sharding_model_specs.emplace_back(std::move(model));
      }
    } else {
      m_model_specs.emplace_back(std::move(model));
    }
  }
}

std::string ModelMerger::s_mapping_file;
std::string Model::s_outdir;

void TypeErasurePass::run_pass(DexStoresVector& stores,
                               ConfigFiles& cfg,
                               PassManager& mgr) {
  // Type mapping file
  ModelMerger::s_mapping_file = cfg.metafile(m_merged_type_mapping_file);
  Model::s_outdir = cfg.get_outdir();

  // Setup Interdex plugin if any models.
  if (m_dex_sharding_model_specs.size() > 0) {
    interdex::InterDexRegistry* registry =
        static_cast<interdex::InterDexRegistry*>(
            PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
    std::function<interdex::InterDexPassPlugin*()> fn =
        [&]() -> interdex::InterDexPassPlugin* {
      return new TypeErasureInterDexPlugin(m_dex_sharding_model_specs, mgr);
    };
    registry->register_plugin("TYPE_ERASURE_PLUGIN", std::move(fn));
  }

  if (m_model_specs.empty()) {
    return;
  }
  auto scope = build_class_scope(stores);
  Model::build_interdex_groups(&cfg);
  for (const auto& model_spec : m_model_specs) {
    if (model_spec.enabled) {
      if (is_interface(type_class(model_spec.root)))
        handle_interface_as_root(const_cast<ModelSpec&>(model_spec), scope,
                                 stores);
      erase_model(model_spec, scope, mgr, stores, cfg);
    }
  }
  post_dexen_changes(scope, stores);
}

ModelMerger* TypeErasurePass::get_model_merger() { return new ModelMerger; }

void TypeErasurePass::erase_model(const ModelSpec& spec,
                                  Scope& scope,
                                  PassManager& mgr,
                                  DexStoresVector& stores,
                                  ConfigFiles& cfg) {
  TRACE(TERA, 2, "[TERA] erasing %s model\n", spec.name.c_str());
  always_assert(!is_interface(type_class(spec.root)));
  auto model = Model::build_model(scope, stores, spec, cfg);
  model.update_redex_stats(mgr);

  auto mm = get_model_merger();
  auto merger_classes =
      mm->merge_model(scope, stores, model, m_max_num_dispatch_target);
  mm->update_redex_stats(spec.class_name_prefix, mgr);

  delete mm;
}

static TypeErasurePass s_pass;
