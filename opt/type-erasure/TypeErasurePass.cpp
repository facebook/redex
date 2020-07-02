/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeErasurePass.h"

#include "ClassMerging.h"
#include "DexUtil.h"

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

void load_types(const std::vector<std::string>& type_names,
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

void load_types_and_prefixes(const std::vector<std::string>& type_names,
                             std::unordered_set<DexType*>& types,
                             std::unordered_set<std::string>& prefixes) {
  for (const auto& type_s : type_names) {
    auto target_type = get_type(type_s);
    if (target_type == nullptr) {
      prefixes.insert(type_s);
    } else {
      types.insert(target_type);
    }
  }
}

void load_types(const std::vector<std::string>& type_names, TypeSet& types) {
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

  if (model_spec.roots.empty()) {
    fprintf(stderr,
            "[TERA] Wrong specification: model %s must have \"roots\"\n",
            model_spec.name.c_str());
    return false;
  }

  for (const auto root : model_spec.roots) {
    if (root == nullptr) {
      fprintf(stderr,
              "[TERA] Wrong specification: model %s must have \"roots\"\n",
              model_spec.name.c_str());
      return false;
    }
  }

  return true;
}

InterDexGroupingType get_merge_per_interdex_type(
    const std::string& merge_per_interdex_set) {

  const static std::unordered_map<std::string, InterDexGroupingType>
      string_to_grouping = {{"disabled", InterDexGroupingType::DISABLED},
                            {"non-hot-set", InterDexGroupingType::NON_HOT_SET},
                            {"full", InterDexGroupingType::FULL}};

  always_assert_log(string_to_grouping.count(merge_per_interdex_set) > 0,
                    "InterDex Grouping Type %s not found. Please check the list"
                    " of accepted values.",
                    merge_per_interdex_set.c_str());
  return string_to_grouping.at(merge_per_interdex_set);
}

TypeTagConfig get_type_tag_config(const std::string& type_tag_config) {
  const static std::unordered_map<std::string, TypeTagConfig> string_to_config =
      {{"none", TypeTagConfig::NONE},
       {"generate", TypeTagConfig::GENERATE},
       {"input-pass-type-tag-to-ctor",
        TypeTagConfig::INPUT_PASS_TYPE_TAG_TO_CTOR},
       {"input-handled", TypeTagConfig::INPUT_HANDLED}};
  always_assert_log(string_to_config.count(type_tag_config) > 0,
                    "Type tag config type %s not found. Please check the list"
                    " of accepted values.",
                    type_tag_config.c_str());
  TRACE(TERA, 5, "type tag config %s %d", type_tag_config.c_str(),
        string_to_config.at(type_tag_config));
  return string_to_config.at(type_tag_config);
}

} // namespace

void TypeErasurePass::bind_config() {
  bool process_method_meta;
  bind("process_method_meta", false, process_method_meta);
  int64_t max_num_dispatch_target;
  bind("max_num_dispatch_target", {0}, max_num_dispatch_target);
  bool merge_static_methods_within_shape;
  bind("merge_static_methods_within_shape", false,
       merge_static_methods_within_shape);
  bool merge_direct_methods_within_shape;
  bind("merge_direct_methods_within_shape", false,
       merge_direct_methods_within_shape);
  bool merge_nonvirt_methods_within_shape;
  bind("merge_nonvirt_methods_within_shape", false,
       merge_nonvirt_methods_within_shape);

  // load model specifications
  Json::Value models;
  bind("models", Json::Value(), models);

  after_configuration([=] {
    if (max_num_dispatch_target > 0) {
      m_max_num_dispatch_target =
          boost::optional<size_t>(static_cast<size_t>(max_num_dispatch_target));
    }

    if (models.isNull()) return;
    if (!models.isArray()) {
      fprintf(stderr,
              "[TERA] Wrong specification: \"models\" is not an array\n");
      return;
    }

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
      std::string type_tag_config;
      model_spec.get("type_tag_config", "generate", type_tag_config);
      model.type_tag_config = get_type_tag_config(type_tag_config);
      size_t min_count;
      model_spec.get("min_count", 2, min_count);
      model.min_count = min_count > 0 ? min_count : 0;
      model_spec.get("name", "", model.name);
      std::vector<std::string> root_names;
      model_spec.get("roots", {}, root_names);
      load_types(root_names, model.roots);
      std::vector<std::string> excl_names;
      model_spec.get("exclude", {}, excl_names);
      load_types_and_prefixes(excl_names, model.exclude_types,
                              model.exclude_prefixes);
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

        std::vector<std::string> gen_anno_names;
        gen_spec.get("annos", {}, gen_anno_names);
        load_types(gen_anno_names, model.gen_annos);
      }
      model_spec.get("include_primary_dex", false, model.include_primary_dex);

      std::string merge_per_interdex_set;
      model_spec.get("merge_per_interdex_set", "disabled",
                     merge_per_interdex_set);
      model.merge_per_interdex_set =
          get_merge_per_interdex_type(merge_per_interdex_set);

      always_assert_log(!model.merge_per_interdex_set ||
                            (model.type_tag_config != TypeTagConfig::NONE),
                        "Cannot group when type tag is not needed.");

      size_t max_count;
      model_spec.get("max_count", 0, max_count);
      Json::Value approximate_shaping;
      model_spec.get("approximate_shape_merging", Json::Value(),
                     model.approximate_shape_merging);
      model_spec.get("merge_types_with_static_fields", false,
                     model.merge_types_with_static_fields);
      model_spec.get("keep_debug_info", false, model.keep_debug_info);
      model_spec.get("replace_type_like_const_strings", true,
                     model.replace_type_like_const_strings);
      model_spec.get("exclude_reference_to_android_sdk", Json::Value(),
                     model.exclude_reference_to_android_sdk);
      if (max_count > 0) {
        model.max_count = boost::optional<size_t>(max_count);
      }
      model.process_method_meta = process_method_meta;
      model.merge_static_methods_within_shape =
          merge_static_methods_within_shape;
      model.merge_direct_methods_within_shape =
          merge_direct_methods_within_shape;
      model.merge_nonvirt_methods_within_shape =
          merge_nonvirt_methods_within_shape;
      model.max_num_dispatch_target = m_max_num_dispatch_target;

      if (!verify_model_spec(model)) {
        continue;
      }

      m_model_specs.emplace_back(std::move(model));
    }

    TRACE(TERA, 1, "[TERA] valid model specs %ld", m_model_specs.size());
  });
}

void TypeErasurePass::run_pass(DexStoresVector& stores,
                               ConfigFiles& conf,
                               PassManager& mgr) {
  if (m_model_specs.empty()) {
    return;
  }
  class_merging::set_up(conf);
  auto scope = build_class_scope(stores);
  for (ModelSpec& model_spec : m_model_specs) {
    if (!model_spec.enabled) {
      continue;
    }
    class_merging::merge_model(scope, mgr, stores, model_spec);
  }
  post_dexen_changes(scope, stores);
}

static TypeErasurePass s_pass;
