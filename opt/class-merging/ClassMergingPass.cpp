/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassMergingPass.h"

#include "ClassAssemblingUtils.h"
#include "ClassMerging.h"
#include "ConfigFiles.h"
#include "ConfigUtils.h"
#include "DexUtil.h"
#include "MergingStrategies.h"
#include "Show.h"
#include "Trace.h"

using namespace class_merging;

namespace {

template <typename Types>
void load_types(const std::vector<std::string>& type_names, Types& types) {
  std::vector<DexType*> ts = utils::get_types(type_names);
  for (const auto& t : ts) {
    const auto& cls = type_class(t);
    if (cls == nullptr) {
      TRACE(CLMG, 2, "Missing definition for type %s", SHOW(t));
      types.clear();
      return;
    }
    types.insert(t);
  }
}

/**
 * Verify model specs are consistent
 */
bool verify_model_spec(const ModelSpec& model_spec) {
  always_assert_log(
      !model_spec.name.empty(),
      "[ClassMerging] Wrong specification: model must have \"name\"");
  always_assert_log(!model_spec.class_name_prefix.empty(),
                    "[ClassMerging] Wrong specification: model %s must have "
                    "\"class_name_prefix\"",
                    model_spec.name.c_str());

  if (model_spec.roots.empty()) {
    // To share the configurations easily across apps, we ignore the models
    // without roots.
    TRACE(CLMG, 2,
          "[ClassMerging] Wrong specification: model %s must have \"roots\"",
          model_spec.name.c_str());
    return false;
  }

  for (const auto root : model_spec.roots) {
    always_assert_log(
        root,
        "[ClassMerging] Wrong specification: model %s must have \"roots\"",
        model_spec.name.c_str());
  }
  return true;
}

InterDexGroupingType get_merge_per_interdex_type(
    const std::string& merge_per_interdex_set) {

  const static std::unordered_map<std::string, InterDexGroupingType>
      string_to_grouping = {
          {"disabled", InterDexGroupingType::DISABLED},
          {"non-hot-set", InterDexGroupingType::NON_HOT_SET},
          {"non-ordered-set", InterDexGroupingType::NON_ORDERED_SET},
          {"full", InterDexGroupingType::FULL}};

  always_assert_log(string_to_grouping.count(merge_per_interdex_set) > 0,
                    "InterDex Grouping Type %s not found. Please check the list"
                    " of accepted values.",
                    merge_per_interdex_set.c_str());
  return string_to_grouping.at(merge_per_interdex_set);
}

strategy::Strategy get_merging_strategy(const std::string& merging_strategy) {
  const static std::unordered_map<std::string, strategy::Strategy>
      string_to_strategy = {
          {"by_cls_count", strategy::Strategy::BY_CLASS_COUNT},
          {"by_code_size", strategy::Strategy::BY_CODE_SIZE},
          {"by_refs", strategy::Strategy::BY_REFS}};

  always_assert_log(string_to_strategy.count(merging_strategy) > 0,
                    "Merging strategy %s not found. Please check the list"
                    " of accepted values.",
                    merging_strategy.c_str());
  return string_to_strategy.at(merging_strategy);
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
  TRACE(CLMG, 5, "type tag config %s %d", type_tag_config.c_str(),
        string_to_config.at(type_tag_config));
  return string_to_config.at(type_tag_config);
}

} // namespace

namespace class_merging {

void ClassMergingPass::bind_config() {
  bool process_method_meta;
  bind("process_method_meta", false, process_method_meta);
  int64_t max_num_dispatch_target;
  bind("max_num_dispatch_target", 0, max_num_dispatch_target);
  trait(Traits::Pass::unique, true);

  // load model specifications
  std::vector<Json::Value> models;
  bind("models", {}, models);

  std::string dflt_type_usages_mode;
  bind("default_type_usages_mode", "", dflt_type_usages_mode);

  after_configuration([=] {
    if (max_num_dispatch_target > 0) {
      m_max_num_dispatch_target =
          boost::optional<size_t>(static_cast<size_t>(max_num_dispatch_target));
    }

    if (models.empty()) return;

    auto parse_usage_mode = [](const std::string& s,
                               ModelSpec::TypeUsagesMode dflt) {
      if (s.empty()) {
        return dflt;
      }
      if (s == "all-types") {
        return ModelSpec::TypeUsagesMode::kAllTypeRefs;
      } else if (s == "class-loads") {
        return ModelSpec::TypeUsagesMode::kClassLoads;
      } else if (s == "class-loads-bb") {
        return ModelSpec::TypeUsagesMode::kClassLoadsBasicBlockFiltering;
      } else {
        always_assert_log(false, "Unknown type-usage-mode %s", s.c_str());
      }
    };
    ModelSpec::TypeUsagesMode default_mode =
        parse_usage_mode(dflt_type_usages_mode, ModelSpec().type_usages_mode);

    // load each model spec for erasure
    for (auto it = models.begin(); it != models.end(); ++it) {
      const auto& value = *it;
      always_assert_log(
          value.isObject(),
          "[ClassMerging] Wrong specification: model in array not an object");
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
      utils::load_types_and_prefixes(excl_names, model.exclude_types,
                                     model.exclude_prefixes);
      model_spec.get("class_name_prefix", "", model.class_name_prefix);
      Json::Value generated;
      model_spec.get("generated", Json::Value(), generated);
      if (!generated.isNull()) {
        if (!generated.isObject()) {
          fprintf(stderr,
                  "[ClassMerging] Wrong specification: model in array not an "
                  "object\n");
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

      std::vector<std::string> const_class_safe_names;
      model_spec.get("const_class_safe_types", {}, const_class_safe_names);
      load_types(const_class_safe_names, model.const_class_safe_types);

      model_spec.get("include_primary_dex", false, model.include_primary_dex);

      // Merging strategy is by default `by_cls_count`.
      std::string merging_strategy;
      model_spec.get("merging_strategy", "by_cls_count", merging_strategy);
      model.strategy = get_merging_strategy(merging_strategy);

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
      if (max_count > 0) {
        model.max_count = boost::optional<size_t>(max_count);
      }
      model.process_method_meta = process_method_meta;
      model.max_num_dispatch_target = m_max_num_dispatch_target;

      std::string usage_mode_str =
          model_spec.get("type_usage_mode", std::string(""));
      model.type_usages_mode = parse_usage_mode(usage_mode_str, default_mode);

      if (!verify_model_spec(model)) {
        continue;
      }

      m_model_specs.emplace_back(std::move(model));
    }

    TRACE(CLMG, 2, "[ClassMerging] valid model specs %ld",
          m_model_specs.size());
  });
}

void ClassMergingPass::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  if (m_model_specs.empty()) {
    return;
  }

  auto scope = build_class_scope(stores);
  for (ModelSpec& model_spec : m_model_specs) {
    if (!model_spec.enabled) {
      continue;
    }
    if (conf.force_single_dex() && !model_spec.include_primary_dex) {
      TRACE(CLMG, 2,
            "Change include_primary_dex to true because the apk will be single "
            "dex");
      model_spec.include_primary_dex = true;
    }
    // TODO: We will move the logic of collecting mergeables outside of Model
    // building. Then this step can be removed.
    handle_interface_as_root(model_spec, scope, stores);
    for (const auto root : model_spec.roots) {
      always_assert(!is_interface(type_class(root)));
    }
    class_merging::merge_model(scope, conf, mgr, stores, model_spec);
  }
  post_dexen_changes(scope, stores);
}

static ClassMergingPass s_pass;

} // namespace class_merging
