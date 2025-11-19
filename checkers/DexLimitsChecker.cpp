/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexLimitsChecker.h"

#include <iterator>
#include <optional>
#include <sstream>

#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLimitsInfo.h"
#include "DexUtil.h"
#include "InitClassesWithSideEffects.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"

namespace redex_properties {

namespace {

using dex_data_map_t =
    UnorderedMap<std::string, std::vector<DexLimitsChecker::DexData>>;

template <typename T>
std::vector<T> extract(const UnorderedMap<T, size_t>& input) {
  std::vector<T> tmp{};
  tmp.reserve(input.size());
  unordered_transform(input, std::back_inserter(tmp),
                      [](auto& p) { return p.first; });
  return tmp;
}

// Avoid sorting overhead, but still store in vector.
template <class Collection,
          class Value =
              typename std::remove_const<typename Collection::value_type>::type>
std::vector<Value> unordered_to_vector(Collection& collection) {
  std::vector<Value> result;
  result.reserve(collection.size());
  for (auto& entry : UnorderedIterable(collection)) {
    result.emplace_back(entry);
  }
  return result;
}

dex_data_map_t create_data(
    DexStoresVector& stores,
    init_classes::InitClassesWithSideEffects* init_classes) {
  dex_data_map_t tmp;

  for (const auto& store : stores) {
    std::vector<DexLimitsChecker::DexData> dexes_data;
    dexes_data.reserve(store.num_dexes());

    for (const auto& classes : store.get_dexen()) {
      DexLimitsInfo dex_limits(init_classes);
      for (const auto& cls : classes) {
        dex_limits.update_refs_by_always_adding_class(cls);
      }

      const auto& dex_struct = dex_limits.get_dex();

      dexes_data.emplace_back(DexLimitsChecker::DexData{
          extract(dex_struct.get_frefs()),
          extract(dex_struct.get_mrefs()),
          extract(dex_struct.get_trefs()),
          dex_struct.get_pending_init_class_fields(),
          dex_struct.get_pending_init_class_types(),
      });
    }
    tmp.emplace(store.get_name(), std::move(dexes_data));
  }

  return tmp;
}

struct IssueIndex {
  const DexStore* store{nullptr};
  size_t dex_id{0};
  bool field_overflow{false};
  bool method_overflow{false};
  bool type_overflow{false};
};

template <typename T>
UnorderedSet<T> to_set(const std::vector<T>& in) {
  return UnorderedSet<T>{in.begin(), in.end()};
}

std::string print_new_entries(const dex_data_map_t& old_map_vec,
                              const dex_data_map_t& new_map_vec,
                              const std::vector<IssueIndex>& issues) {
  std::ostringstream oss;

  struct DexDataWithSet {
    // These should be sets, but for overhead reasons we keep these as vectors
    // and only translate them when we find an issue.
    UnorderedSet<DexFieldRef*> fields;
    UnorderedSet<DexMethodRef*> methods;
    UnorderedSet<const DexType*> types;
    // Share memory. Lifetimes are OK because confined to this function.
    const UnorderedSet<const DexType*>& pending_init_class_fields;
    const UnorderedSet<const DexType*>& pending_init_class_types;

    DexDataWithSet(UnorderedSet<DexFieldRef*> f,
                   UnorderedSet<DexMethodRef*> m,
                   UnorderedSet<const DexType*> t,
                   const UnorderedSet<const DexType*>& p_f,
                   const UnorderedSet<const DexType*>& p_t)
        : fields(std::move(f)),
          methods(std::move(m)),
          types(std::move(t)),
          pending_init_class_fields(p_f),
          pending_init_class_types(p_t) {}
  };
  auto to_set_type = [](const auto& in) {
    UnorderedMap<std::string, std::vector<DexDataWithSet>> ret;
    for (const auto& p : UnorderedIterable(in)) {
      std::vector<DexDataWithSet> tmp_vec;
      tmp_vec.reserve(p.second.size());
      for (const auto& vec_elem : p.second) {
        tmp_vec.emplace_back(to_set(vec_elem.fields),
                             to_set(vec_elem.methods),
                             to_set(vec_elem.types),
                             vec_elem.pending_init_class_fields,
                             vec_elem.pending_init_class_types);
      }
      ret.emplace(p.first, std::move(tmp_vec));
    }
    return ret;
  };
  auto old_map = to_set_type(old_map_vec);
  auto new_map = to_set_type(new_map_vec);

  for (const auto& i : issues) {
    const auto& store_name = i.store->get_name();
    auto st_it = old_map.find(store_name);
    if (st_it == old_map.end()) {
      // Totally new store, log that.
      oss << "\nStore " << store_name << " is newly created.\n";
      continue;
    }

    // See whether we have the dex before. This may not match when dexes are
    // deleted, best effort really.
    auto& old_dexes = st_it->second;
    if (old_dexes.size() <= i.dex_id) {
      oss << "\nStore " << store_name << " dex " << i.dex_id
          << " seems newly created.\n";
      continue;
    }

    auto new_st_it = new_map.find(store_name);
    redex_assert(new_st_it != new_map.cend());

    auto& new_dexes = new_st_it->second;
    redex_assert(new_dexes.size() > i.dex_id);

    auto print_differences = [&](const auto& old_data, const auto& new_data,
                                 const char* prefix) {
      // Won't be sorted, but sorting would be a template pain.
      bool have_changes = false;
      for (auto* entry : UnorderedIterable(new_data)) {
        if (old_data.count(entry) != 0) {
          continue;
        }
        if (!have_changes) {
          have_changes = true;
          oss << prefix << show(entry);
        } else {
          oss << ", " << show(entry);
        }
      }
      if (have_changes) {
        oss << "\n";
      }
      return have_changes;
    };

    bool had_fields{false};
    if (i.field_overflow) {
      had_fields =
          print_differences(old_dexes[i.dex_id].fields,
                            new_dexes[i.dex_id].fields,
                            "Fields: ") ||
          print_differences(old_dexes[i.dex_id].pending_init_class_fields,
                            new_dexes[i.dex_id].pending_init_class_fields,
                            "Pending init-class Fields For: ");
      if (!had_fields) {
        oss << "Failed detecting field changes for " << store_name << "@"
            << i.dex_id << "\n";
      }
    }
    bool had_methods{false};
    if (i.method_overflow) {
      had_methods = print_differences(old_dexes[i.dex_id].methods,
                                      new_dexes[i.dex_id].methods,
                                      "Methods: ");
      if (!had_methods) {
        oss << "Failed detecting method changes for " << store_name << "@"
            << i.dex_id << "\n";
      }
    }
    bool had_types{false};
    if (i.type_overflow) {
      had_types =
          print_differences(old_dexes[i.dex_id].types,
                            new_dexes[i.dex_id].types,
                            "Types: ") ||
          print_differences(old_dexes[i.dex_id].pending_init_class_types,
                            new_dexes[i.dex_id].pending_init_class_types,
                            "Pending init-class Types: ");
      if (!had_types) {
        oss << "Failed detecting type changes for " << store_name << "@"
            << i.dex_id << "\n";
      }
    }
    if (!had_fields && !had_methods && !had_types) {
      // Run the other things, maybe there's a misdetection.
      if (!i.field_overflow) {
        print_differences(old_dexes[i.dex_id].fields,
                          new_dexes[i.dex_id].fields, "Fields: ");
        print_differences(old_dexes[i.dex_id].pending_init_class_fields,
                          new_dexes[i.dex_id].pending_init_class_fields,
                          "Pending init-class Fields For: ");
      }
      if (!i.method_overflow) {
        print_differences(old_dexes[i.dex_id].methods,
                          new_dexes[i.dex_id].methods,
                          "Methods: ");
      }
      if (!i.type_overflow) {
        print_differences(old_dexes[i.dex_id].types, new_dexes[i.dex_id].types,
                          "Types: ");
        print_differences(old_dexes[i.dex_id].pending_init_class_types,
                          new_dexes[i.dex_id].pending_init_class_types,
                          "Pending init-class Types: ");
      }
    }
  }

  return oss.str();
}

} // namespace

void DexLimitsChecker::run_checker(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr,
                                   bool established) {
  if (!established) {
    return;
  }
  Timer t("ref_validation");
  std::string pass_name = "initial state";
  const auto* info = mgr.get_current_pass_info();
  if (info != nullptr) {
    pass_name = info->name;
  }

  std::ostringstream result;
  Scope scope = build_class_scope(stores);
  std::unique_ptr<init_classes::InitClassesWithSideEffects>
      init_classes_with_side_effects;
  if (!mgr.init_class_lowering_has_run()) {
    init_classes_with_side_effects =
        std::make_unique<init_classes::InitClassesWithSideEffects>(
            scope, conf.create_init_class_insns());
  }

  auto check_ref_num = [&init_classes_with_side_effects, &pass_name,
                        &result](const DexClasses& classes,
                                 const DexStore& store,
                                 size_t dex_id) -> std::optional<IssueIndex> {
    DexLimitsInfo dex_limits(init_classes_with_side_effects.get());
    bool field_overflow{false};
    bool method_overflow{false};
    bool type_overflow{false};
    for (const auto& cls : classes) {
      if (!dex_limits.update_refs_by_adding_class(cls)) {
        method_overflow |= dex_limits.is_method_overflow();
        field_overflow |= dex_limits.is_field_overflow();
        type_overflow |= dex_limits.is_type_overflow();
      }
    }
    auto add_overflow_msg = [&](bool check, const char* type_str) {
      if (check) {
        result << pass_name << " adds too many " << type_str << " refs in dex "
               << dex_name(store, dex_id) << "\n";
      }
    };
    add_overflow_msg(field_overflow, "field");
    add_overflow_msg(method_overflow, "method");
    add_overflow_msg(type_overflow, "type");

    if (field_overflow || method_overflow || type_overflow) {
      TRACE(PM,
            0,
            "Recording overflow %d / %d / %d",
            field_overflow,
            method_overflow,
            type_overflow);
      return IssueIndex{&store, dex_id, field_overflow, method_overflow,
                        type_overflow};
    }

    return std::nullopt;
  };

  std::vector<IssueIndex> issues;
  for (const auto& store : stores) {
    size_t dex_id = 0;
    for (const auto& classes : store.get_dexen()) {
      auto maybe_issue = check_ref_num(classes, store, dex_id++);
      if (maybe_issue) {
        issues.emplace_back(*maybe_issue);
      }
    }
  }

  auto old_data = std::move(m_data);
  m_data = create_data(stores, init_classes_with_side_effects.get());
  TRACE(PM, 0, "%s", result.str().c_str());
  always_assert_log(issues.empty(),
                    "%s\n%s",
                    result.str().c_str(),
                    print_new_entries(old_data, m_data, issues).c_str());
}

} // namespace redex_properties

namespace {
static redex_properties::DexLimitsChecker s_checker;
} // namespace
