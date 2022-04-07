/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AppModuleUsage.h"

#include <algorithm>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <sstream>
#include <string>

#include "ConfigFiles.h"
#include "CppUtil.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
constexpr const char* APP_MODULE_USAGE_OUTPUT_FILENAME =
    "redex-app-module-usage.txt";
constexpr const char* APP_MODULE_COUNT_OUTPUT_FILENAME =
    "redex-app-module-count.csv";
constexpr const char* USES_AM_ANNO_VIOLATIONS_FILENAME =
    "redex-app-module-annotation-violations.csv";

void write_violations_to_file(const app_module_usage::Violations& violations,
                              const std::string& path) {
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& [entrypoint, modules] : violations) {
    ofs << entrypoint;
    for (const auto& module : modules) {
      ofs << ", " << module;
    }
    ofs << std::endl;
  }
  ofs.close();
}

void write_method_module_usages_to_file(
    const app_module_usage::MethodStoresReferenced& method_store_refs,
    const ConcurrentMap<DexType*, DexStore*>& type_store_map,
    const std::string& path) {

  TRACE(APP_MOD_USE, 4, "Outputting module usages at %s", path.c_str());
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& [method, store_refs] : method_store_refs) {
    if (store_refs.empty()) {
      continue;
    }
    auto it = type_store_map.find(method->get_class());
    if (it != type_store_map.end()) {
      ofs << "(" << it->second->get_name() << ") ";
    }
    ofs << show(method);
    for (const auto& [store, refl_only] : store_refs) {
      ofs << ", ";
      if (refl_only) {
        ofs << "(reflection) ";
      }
      ofs << store->get_name();
    }
    ofs << std::endl;
  }
  ofs.close();
}

void write_app_module_use_stats(
    const app_module_usage::MethodStoresReferenced& method_store_refs,
    const std::string& path) {

  struct ModuleUseCount {
    unsigned int direct_count{0};
    unsigned int reflective_count{0};
  };

  std::unordered_map<DexStore*, ModuleUseCount> counts;

  for (const auto& [method, store_refs] : method_store_refs) {
    for (const auto& [store, refl_only] : store_refs) {
      auto& count = counts[store];
      if (refl_only) {
        count.reflective_count++;
      } else {
        count.direct_count++;
      }
    }
  }

  TRACE(APP_MOD_USE, 4, "Outputting module use count at %s", path.c_str());
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& [module, use_count] : counts) {
    ofs << module->get_name() << ", " << use_count.direct_count << ", "
        << use_count.reflective_count << std::endl;
  }
  ofs.close();
}
} // namespace

void AppModuleUsagePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {

  if (!m_uses_app_module_annotation) {
    fprintf(
        stderr,
        "WARNING: Annotation class not found. Skipping AppModuleUsagePass.");
    return;
  }

  for (auto& store : stores) {
    Scope scope = build_class_scope(store.get_dexen());
    walk::classes(scope, [&](DexClass* cls) {
      m_type_store_map.emplace(cls->get_type(), &store);
    });
  }

  load_preexisting_violations(stores);

  const auto& full_scope = build_class_scope(stores);
  // TODO: Remove classes from scope that are exempt from checking.
  auto method_store_refs = analyze_method_xstore_references(full_scope);
  auto field_store_refs = analyze_field_xstore_references(full_scope);

  if (m_output_module_use) {
    auto module_use_path = conf.metafile(APP_MODULE_USAGE_OUTPUT_FILENAME);
    write_method_module_usages_to_file(method_store_refs, m_type_store_map,
                                       module_use_path);

    auto module_count_path = conf.metafile(APP_MODULE_COUNT_OUTPUT_FILENAME);
    write_app_module_use_stats(method_store_refs, module_count_path);
  }

  app_module_usage::Violations violations;
  auto num_violations =
      gather_violations(method_store_refs, field_store_refs, violations);
  auto report_path = conf.metafile(USES_AM_ANNO_VIOLATIONS_FILENAME);
  write_violations_to_file(violations, report_path);

  mgr.set_metric("num_methods_access_app_module", method_store_refs.size());
  mgr.set_metric("num_violations", num_violations);

  if (m_crash_with_violations) {
    always_assert_log(num_violations == 0,
                      "There are @UsesAppModule violations. See %s \n",
                      report_path.c_str());
  }
}

void AppModuleUsagePass::load_preexisting_violations(DexStoresVector& stores) {
  if (m_preexisting_violations_filepath.empty()) {
    TRACE(APP_MOD_USE, 1, "No preexisting violations provided.");
    return;
  }
  std::ifstream ifs(m_preexisting_violations_filepath);
  if (!ifs.is_open()) {
    fprintf(stderr,
            "WARNING: Could not open preexisting violations at \"%s\"\n",
            m_preexisting_violations_filepath.c_str());
    return;
  }

  // To quickly look up wich DexStore ("module") a name represents
  std::unordered_map<std::string, DexStore*> names_to_stores;
  for (auto& store : stores) {
    names_to_stores.emplace(store.get_name(), &store);
  }

  std::string line;
  while (getline(ifs, line)) {
    boost::optional<std::string> entrypoint;
    for (std::string_view csv_component : split_string(line, ",")) {
      csv_component = trim_whitespaces(csv_component);
      if (!entrypoint) {
        entrypoint = std::string(csv_component);
      } else {
        std::string store_name(csv_component);
        auto it = names_to_stores.find(store_name);
        if (it != names_to_stores.end()) {
          m_preexisting_violations[*entrypoint].emplace(it->second);
        } else {
          TRACE(APP_MOD_USE, 2,
                "Module %s from preexisting violations no longer exists.",
                store_name.c_str());
        }
      }
    }
  }
  ifs.close();
}

ConcurrentMap<DexMethod*, app_module_usage::StoresReferenced>
AppModuleUsagePass::analyze_method_xstore_references(const Scope& scope) {

  auto get_type_ref_for_insn = [](IRInstruction* insn) -> DexType* {
    if (insn->has_method()) {
      return insn->get_method()->get_class();
    } else if (insn->has_field()) {
      return insn->get_field()->get_class();
    } else if (insn->has_type()) {
      return insn->get_type();
    }
    return nullptr;
  };

  ConcurrentMap<DexMethod*, app_module_usage::StoresReferenced>
      method_store_refs;
  reflection::MetadataCache refl_metadata_cache;

  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    const auto* method_store = m_type_store_map.at(method->get_class());
    std::unique_ptr<reflection::ReflectionAnalysis> analysis =
        std::make_unique<reflection::ReflectionAnalysis>(
            /* dex_method */ method,
            /* context (interprocedural only) */ nullptr,
            /* summary_query_fn (interprocedural only) */ nullptr,
            /* metadata_cache */ &refl_metadata_cache);

    auto get_reflective_type_ref_for_insn =
        [&analysis](IRInstruction* insn) -> DexType* {
      if (!opcode::is_an_invoke(insn->opcode())) {
        // If an object type is from reflection it will be in the
        // RESULT_REGISTER for some instruction.
        const auto& o = analysis->get_abstract_object(RESULT_REGISTER, insn);
        if (o &&
            (o.get().obj_kind != reflection::CLASS ||
             (analysis->get_class_source(RESULT_REGISTER, insn).has_value() &&
              analysis->get_class_source(RESULT_REGISTER, insn).get() ==
                  reflection::REFLECTION))) {
          // If the obj is a CLASS then it must have a class source of
          // REFLECTION
          return o.get().dex_type;
        }
      }
      return nullptr;
    };

    auto get_store_if_access_is_xstore = [&](DexType* to) -> DexStore* {
      // The type may be external.
      auto it = m_type_store_map.find(to);
      if (it == m_type_store_map.end()) {
        return nullptr;
      }
      auto store = it->second;
      if (!store->is_root_store() && store != method_store) {
        return store;
      }
      return nullptr;
    };

    app_module_usage::StoresReferenced stores_referenced;
    for (const auto& mie : InstructionIterable(code)) {
      IRInstruction* insn = mie.insn;
      auto maybe_type_ref = get_type_ref_for_insn(insn);
      if (maybe_type_ref) {
        auto maybe_store = get_store_if_access_is_xstore(maybe_type_ref);
        if (maybe_store) {
          // Creates the value if it doesn't exist.
          stores_referenced[maybe_store] = false /* used_only_reflectively */;
        }
      }
      auto maybe_refl_type_ref = get_reflective_type_ref_for_insn(insn);
      if (maybe_refl_type_ref) {
        auto maybe_store = get_store_if_access_is_xstore(maybe_refl_type_ref);
        if (maybe_store) {
          if (!stores_referenced.count(maybe_store)) {
            stores_referenced.emplace(maybe_store, true);
          }
          // If the entry already exists, doesn't matter if we add it because
          // |= true is always no-op.
        }
      }
    }

    if (!stores_referenced.empty()) {
      method_store_refs.emplace(method, std::move(stores_referenced));
    }
  });

  return method_store_refs;
}

ConcurrentMap<DexField*, DexStore*>
AppModuleUsagePass::analyze_field_xstore_references(const Scope& scope) {
  ConcurrentMap<DexField*, DexStore*> ret;
  walk::parallel::fields(scope, [&](DexField* field) {
    auto field_store = m_type_store_map.at(field->get_class());

    auto field_type = field->get_type();
    auto it = m_type_store_map.find(field_type);
    if (it == m_type_store_map.end()) {
      // Type may be external.
      return;
    }
    auto store = it->second;
    if (!store->is_root_store() && store != field_store) {
      ret.emplace(field, store);
    }
  });
  return ret;
}

unsigned AppModuleUsagePass::gather_violations(
    const app_module_usage::MethodStoresReferenced& method_store_refs,
    const ConcurrentMap<DexField*, DexStore*>& field_store_refs,
    app_module_usage::Violations& violations) const {

  unsigned n_violations{0u};
  for (const auto& [method, stores_referenced] : method_store_refs) {
    auto method_name = show(method);
    for (const auto& [store, only_reflection] : stores_referenced) {
      if (access_granted_by_annotation(method, store)) {
        continue;
      }
      if (access_excused_due_to_preexisting(show(method), store)) {
        continue;
      }
      TRACE(
          APP_MOD_USE,
          0,
          "%s (from module \"%s\") uses app module \"%s\" without annotation\n",
          method_name.c_str(),
          m_type_store_map.at(method->get_class())->get_name().c_str(),
          store->get_name().c_str());
      violations[method_name].emplace(store->get_name());
      n_violations++;
    }
  }

  for (const auto& [field, store] : field_store_refs) {
    auto field_name = show(field);
    if (access_granted_by_annotation(field, store)) {
      continue;
    }
    if (access_excused_due_to_preexisting(show(field), store)) {
      continue;
    }
    TRACE(APP_MOD_USE,
          0,
          "%s (from module \"%s\") uses app module \"%s\" without annotation\n",
          field_name.c_str(),
          m_type_store_map.at(field->get_class())->get_name().c_str(),
          store->get_name().c_str());
    violations[field_name].emplace(store->get_name());
    n_violations++;
  }
  return n_violations;
}

template <typename T>
std::unordered_set<std::string_view> AppModuleUsagePass::get_modules_used(
    T* entrypoint, DexType* annotation_type) {
  std::unordered_set<std::string_view> modules = {};
  auto anno_set = entrypoint->get_anno_set();
  if (anno_set) {
    for (const auto& annotation : anno_set->get_annotations()) {
      if (annotation->type() == annotation_type) {
        for (const DexAnnotationElement& anno_elem : annotation->anno_elems()) {
          always_assert(anno_elem.string->str() == "value");
          always_assert(anno_elem.encoded_value->evtype() == DEVT_ARRAY);
          const auto* array = static_cast<const DexEncodedValueArray*>(
              anno_elem.encoded_value.get());
          for (const auto& value : *(array->evalues())) {
            always_assert(value->evtype() == DEVT_STRING);
            modules.emplace(
                ((DexEncodedValueString*)value.get())->string()->str());
          }
        }
        break;
      }
    }
  }
  return modules;
}

template std::unordered_set<std::string_view>
AppModuleUsagePass::get_modules_used<DexMethod>(DexMethod*, DexType*);

template std::unordered_set<std::string_view>
AppModuleUsagePass::get_modules_used<DexField>(DexField*, DexType*);

template std::unordered_set<std::string_view>
AppModuleUsagePass::get_modules_used<DexClass>(DexClass*, DexType*);

bool AppModuleUsagePass::access_excused_due_to_preexisting(
    const std::string& entrypoint_name, DexStore* store_used) const {
  auto it = m_preexisting_violations.find(entrypoint_name);
  if (it != m_preexisting_violations.end()) {
    return it->second.count(store_used);
  }
  return false;
}

bool AppModuleUsagePass::access_granted_by_annotation(DexMethod* method,
                                                      DexStore* target) const {
  if (get_modules_used(method, m_uses_app_module_annotation)
          .count(target->get_name())) {
    return true;
  }
  return access_granted_by_annotation(type_class(method->get_class()), target);
}

bool AppModuleUsagePass::access_granted_by_annotation(DexField* field,
                                                      DexStore* target) const {
  if (get_modules_used(field, m_uses_app_module_annotation)
          .count(target->get_name())) {
    return true;
  }
  return access_granted_by_annotation(type_class(field->get_class()), target);
}

bool AppModuleUsagePass::access_granted_by_annotation(DexClass* cls,
                                                      DexStore* target) const {
  if (!cls) {
    return false;
  }
  if (get_modules_used(cls, m_uses_app_module_annotation)
          .count(target->get_name())) {
    return true;
  }

  // Check outer class.
  std::string_view cls_name = cls->str();
  auto dollar_sign_idx = cls_name.rfind("$");
  while (dollar_sign_idx != std::string_view::npos) {
    cls_name.remove_suffix(cls_name.size() - dollar_sign_idx);
    std::string new_class_name = std::string(cls_name) + ";";
    auto* ty = DexType::get_type(new_class_name);
    DexClass* outer_class = ty ? type_class(ty) : nullptr;
    if (outer_class) {
      return access_granted_by_annotation(outer_class, target);
    }
    dollar_sign_idx = cls_name.rfind("$");
  }
  return false;
}

static AppModuleUsagePass s_pass;
