/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDex.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "Creators.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPassPlugin.h"
#include "ReachableClasses.h"
#include "StringUtil.h"
#include "Walkers.h"
#include "file-utils.h"

namespace {

constexpr const char* CANARY_PREFIX = "Lsecondary/dex";
constexpr const char CANARY_CLASS_FORMAT[] = "Lsecondary/dex%02d/Canary;";
constexpr size_t CANARY_CLASS_BUFSIZE = sizeof(CANARY_CLASS_FORMAT);

constexpr const char* END_MARKER_FORMAT = "LDexEndMarker";
constexpr const char* SCROLL_MARKER_FORMAT = "LScrollList";

constexpr size_t MAX_DEX_NUM = 99;

constexpr const interdex::DexInfo EMPTY_DEX_INFO = interdex::DexInfo();

std::unordered_set<DexClass*> find_unrefenced_coldstart_classes(
    const Scope& scope,
    const std::vector<DexType*>& interdex_types,
    bool static_prune_classes) {
  int old_no_ref = -1;
  int new_no_ref = 0;

  std::unordered_set<DexType*> coldstart_classes(interdex_types.begin(),
                                                 interdex_types.end());
  std::unordered_set<DexType*> cold_cold_references;
  std::unordered_set<DexClass*> unreferenced_classes;
  Scope input_scope = scope;

  // Don't do analysis if we're not doing pruning.
  if (!static_prune_classes) {
    return unreferenced_classes;
  }

  while (old_no_ref != new_no_ref) {
    old_no_ref = new_no_ref;
    new_no_ref = 0;
    cold_cold_references.clear();
    walk::code(input_scope,
               [&](DexMethod* meth) {
                 return coldstart_classes.count(meth->get_class()) > 0;
               },
               [&](DexMethod* meth, const IRCode& code) {
                 auto base_cls = meth->get_class();
                 for (auto& mie : InstructionIterable(meth->get_code())) {
                   auto inst = mie.insn;
                   DexType* called_cls = nullptr;
                   if (inst->has_method()) {
                     called_cls = inst->get_method()->get_class();
                   } else if (inst->has_field()) {
                     called_cls = inst->get_field()->get_class();
                   } else if (inst->has_type()) {
                     called_cls = inst->get_type();
                   }
                   if (called_cls != nullptr && base_cls != called_cls &&
                       coldstart_classes.count(called_cls) > 0) {
                     cold_cold_references.insert(called_cls);
                   }
                 }
               });
    for (const auto& cls : scope) {
      // Make sure we don't drop classes which might be
      // called from native code.
      if (!can_rename(cls)) {
        cold_cold_references.insert(cls->get_type());
      }
    }

    // Get all classes in the reference set, even if they are not referenced by
    // opcodes directly.
    for (const auto& cls : input_scope) {
      if (cold_cold_references.count(cls->get_type())) {
        std::vector<DexType*> types;
        cls->gather_types(types);
        for (const auto& type : types) {
          cold_cold_references.insert(type);
        }
      }
    }

    Scope output_scope;
    for (auto& cls : coldstart_classes) {
      if (can_rename(type_class(cls)) && cold_cold_references.count(cls) == 0) {
        new_no_ref++;
        unreferenced_classes.insert(type_class(cls));
      } else {
        output_scope.push_back(type_class(cls));
      }
    }
    TRACE(IDEX, 4, "Found %d classes in coldstart with no references.\n",
          new_no_ref);
    input_scope = output_scope;
  }

  return unreferenced_classes;
}

bool is_canary(DexClass* clazz) {
  const char* cname = clazz->get_type()->get_name()->c_str();
  return strncmp(cname, CANARY_PREFIX, sizeof(CANARY_PREFIX) - 1) == 0;
}

void gather_refs(
    const std::vector<std::unique_ptr<interdex::InterDexPassPlugin>>& plugins,
    const interdex::DexInfo& dex_info,
    const DexClass* cls,
    interdex::MethodRefs* mrefs,
    interdex::FieldRefs* frefs,
    interdex::TypeRefs* trefs,
    std::vector<DexClass*>* erased_classes,
    bool should_not_relocate_methods_of_class) {
  std::vector<DexMethodRef*> method_refs;
  std::vector<DexFieldRef*> field_refs;
  std::vector<DexType*> type_refs;
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(type_refs);

  for (const auto& plugin : plugins) {
    plugin->gather_refs(dex_info, cls, method_refs, field_refs, type_refs,
                        erased_classes, should_not_relocate_methods_of_class);
  }

  mrefs->insert(method_refs.begin(), method_refs.end());
  frefs->insert(field_refs.begin(), field_refs.end());
  trefs->insert(type_refs.begin(), type_refs.end());
}

void print_stats(interdex::DexesStructure* dexes_structure) {
  TRACE(IDEX, 2, "InterDex Stats:\n");
  TRACE(IDEX, 2, "\t dex count: %d\n", dexes_structure->get_num_dexes());
  TRACE(IDEX, 2, "\t secondary dex count: %d\n",
        dexes_structure->get_num_secondary_dexes());
  TRACE(IDEX, 2, "\t coldstart dex count: %d\n",
        dexes_structure->get_num_coldstart_dexes());
  TRACE(IDEX, 2, "\t extendex dex count: %d\n",
        dexes_structure->get_num_extended_dexes());
  TRACE(IDEX, 2, "\t scroll dex count: %d\n",
        dexes_structure->get_num_scroll_dexes());
  TRACE(IDEX, 2, "\t mixedmode dex count: %d\n",
        dexes_structure->get_num_mixedmode_dexes());

  TRACE(IDEX, 2, "Global stats:\n");
  TRACE(IDEX, 2, "\t %lu classes\n", dexes_structure->get_num_classes());
  TRACE(IDEX, 2, "\t %lu mrefs\n", dexes_structure->get_num_mrefs());
  TRACE(IDEX, 2, "\t %lu frefs\n", dexes_structure->get_num_frefs());
  TRACE(IDEX, 2, "\t %lu dmethods\n", dexes_structure->get_num_dmethods());
  TRACE(IDEX, 2, "\t %lu vmethods\n", dexes_structure->get_num_vmethods());
  TRACE(IDEX, 2, "\t %lu mrefs\n", dexes_structure->get_num_mrefs());
}

} // namespace

namespace interdex {

bool InterDex::should_skip_class_due_to_plugin(DexClass* clazz) {
  for (const auto& plugin : m_plugins) {
    if (plugin->should_skip_class(clazz)) {
      TRACE(IDEX, 4, "IDEX: Skipping class :: %s\n", SHOW(clazz));
      return true;
    }
  }

  return false;
}

bool InterDex::should_skip_class_due_to_mixed_mode(const DexInfo& dex_info,
                                                   DexClass* clazz) {
  if (!dex_info.primary && m_mixed_mode_info.is_mixed_mode_class(clazz)) {
    TRACE(IDEX, 4, "IDEX: Skipping mixed mode class :: %s\n", SHOW(clazz));
    return true;
  }

  return false;
}

void InterDex::add_to_scope(DexClass* cls) {
  for (auto& plugin : m_plugins) {
    plugin->add_to_scope(cls);
  }
}

bool InterDex::should_not_relocate_methods_of_class(const DexClass* clazz) {
  for (const auto& plugin : m_plugins) {
    if (plugin->should_not_relocate_methods_of_class(clazz)) {
      TRACE(IDEX, 4, "IDEX: Not relocating methods of class :: %s\n",
            SHOW(clazz));
      return true;
    }
  }

  return false;
}

bool InterDex::emit_class(const DexInfo& dex_info,
                          DexClass* clazz,
                          bool check_if_skip,
                          bool perf_sensitive,
                          std::vector<DexClass*>* erased_classes) {
  if (is_canary(clazz)) {
    // Nothing to do here.
    return false;
  }

  if (m_dexes_structure.has_class(clazz)) {
    TRACE(IDEX, 6, "Trying to re-add class %s!\n", SHOW(clazz));
    return false;
  }

  if (check_if_skip && (should_skip_class_due_to_plugin(clazz) ||
                        should_skip_class_due_to_mixed_mode(dex_info, clazz))) {
    return false;
  }

  if (perf_sensitive) {
    clazz->set_perf_sensitive(true);
  }

  // Calculate the extra method and field refs that we would need to add to
  // the current dex if we defined clazz in it.
  MethodRefs clazz_mrefs;
  FieldRefs clazz_frefs;
  TypeRefs clazz_trefs;
  gather_refs(m_plugins, dex_info, clazz, &clazz_mrefs, &clazz_frefs,
              &clazz_trefs, erased_classes,
              should_not_relocate_methods_of_class(clazz));

  bool fits_current_dex = m_dexes_structure.add_class_to_current_dex(
      clazz_mrefs, clazz_frefs, clazz_trefs, clazz);
  if (!fits_current_dex) {
    flush_out_dex(dex_info);

    // Plugins may maintain internal state after gathering refs, and then they
    // tend to forget that state after flushing out (type erasure,
    // looking at you). So, let's redo gathering of refs here to give
    // plugins a chance to rebuild their internal state.
    clazz_mrefs.clear();
    clazz_frefs.clear();
    clazz_trefs.clear();
    if (erased_classes) erased_classes->clear();
    gather_refs(m_plugins, dex_info, clazz, &clazz_mrefs, &clazz_frefs,
                &clazz_trefs, erased_classes,
                should_not_relocate_methods_of_class(clazz));

    m_dexes_structure.add_class_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                                          clazz);
  }
  return true;
}

void InterDex::emit_primary_dex(
    const DexClasses& primary_dex,
    const std::vector<DexType*>& interdex_types,
    const std::unordered_set<DexClass*>& unreferenced_cls) {

  std::unordered_set<DexClass*> primary_dex_set(primary_dex.begin(),
                                                primary_dex.end());

  DexInfo primary_dex_info;
  primary_dex_info.primary = true;

  size_t coldstart_classes_in_primary = 0;
  size_t coldstart_classes_skipped_in_primary = 0;

  // Sort the primary dex according to interdex order (aka emit first the
  // primary classes that appear in the interdex order, in the order that
  // they appear there).
  for (DexType* type : interdex_types) {
    DexClass* cls = type_class(type);
    if (!cls) {
      continue;
    }

    if (primary_dex_set.count(cls) > 0) {
      if (unreferenced_cls.count(cls) > 0) {
        TRACE(IDEX, 5, "[primary dex]: %s no longer linked to coldstart set.\n",
              SHOW(cls));
        coldstart_classes_skipped_in_primary++;
        continue;
      }

      emit_class(primary_dex_info, cls, /* check_if_skip */ true,
                 /* perf_sensitive */ true);
      coldstart_classes_in_primary++;
    }
  }

  // Now add the rest.
  for (DexClass* cls : primary_dex) {
    emit_class(primary_dex_info, cls, /* check_if_skip */ true,
               /* perf_sensitive */ true);
  }
  TRACE(IDEX, 2,
        "[primary dex]: %d out of %d classes in primary dex "
        "from interdex list.\n",
        coldstart_classes_in_primary, primary_dex.size());
  TRACE(IDEX, 2,
        "[primary dex]: %d out of %d classes in primary dex skipped "
        "from interdex list.\n",
        coldstart_classes_skipped_in_primary, primary_dex.size());

  flush_out_dex(primary_dex_info);

  // Double check only 1 dex was created.
  always_assert_log(
      m_dexes_structure.get_num_dexes() == 1,
      "[error]: Primary dex doesn't fit in only 1 dex anymore :|, but in %d\n",
      m_dexes_structure.get_num_dexes());
}

void InterDex::emit_interdex_classes(
    const std::vector<DexType*>& interdex_types,
    const std::unordered_set<DexClass*>& unreferenced_classes) {
  if (interdex_types.size() == 0) {
    TRACE(IDEX, 2, "No interdex classes passed.\n");
    return;
  }

  DexInfo dex_info;
  // NOTE: coldstart has no interaction with extended and scroll set, but that
  //       is not true for the later 2.
  dex_info.coldstart = true;

  // Last end market delimits where the whole coldstart set ends
  // and the extended coldstart set begins.
  auto last_end_marker_it =
      m_end_markers.size() > 0
          ? std::find(interdex_types.begin(), interdex_types.end(),
                      m_end_markers.back())
          : interdex_types.end();

  auto scroll_list_end_it = interdex_types.end();
  auto scroll_list_start_it = interdex_types.end();

  if (m_scroll_markers.size() > 0) {
    scroll_list_end_it = std::find(interdex_types.begin(), interdex_types.end(),
                                   m_scroll_markers.back());
    scroll_list_start_it = std::find(
        interdex_types.begin(), interdex_types.end(), m_scroll_markers.front());
  }

  size_t cls_skipped_in_secondary = 0;

  for (auto it = interdex_types.begin(); it != interdex_types.end(); ++it) {
    DexType* type = *it;
    DexClass* cls = type_class(type);

    if (!cls) {
      TRACE(IDEX, 5, "[interdex classes]: No such entry %s.\n", SHOW(type));

      auto end_marker =
          std::find(m_end_markers.begin(), m_end_markers.end(), type);
      if (end_marker != m_end_markers.end()) {
        TRACE(IDEX, 2, "Terminating dex due to %s\n", SHOW(type));

        flush_out_dex(dex_info);

        // Check if we need to add the mixed mode classes here.
        if (end_marker == std::prev(m_end_markers.end())) {
          dex_info.coldstart = false;
          dex_info.extended = true;

          if (m_mixed_mode_info.has_predefined_classes()) {
            TRACE(IDEX, 3,
                  "Emitting the mixed mode dex between the coldstart "
                  "set and the extended set of classes.\n");
            bool can_touch_interdex_order =
                m_mixed_mode_info.can_touch_coldstart_set() ||
                m_mixed_mode_info.can_touch_coldstart_extended_set();

            emit_mixed_mode_classes(interdex_types, can_touch_interdex_order);
          }
        }
      }

      if (it == scroll_list_start_it) {
        dex_info.scroll = true;
      } else if (it == scroll_list_end_it) {
        dex_info.scroll = false;
      }

      continue;
    }

    // If we can't touch coldstart classes, simply remove the class
    // from the mix mode class list. Otherwise, we will end up moving
    // the class in the mixed mode dex.
    if (!m_mixed_mode_info.can_touch_coldstart_set() &&
        m_mixed_mode_info.is_mixed_mode_class(cls)) {
      if (last_end_marker_it > it) {
        TRACE(IDEX, 2,
              "%s is part of coldstart classes. Removing it from the "
              "list of mix mode classes\n",
              SHOW(cls));
        m_mixed_mode_info.remove_mixed_mode_class(cls);
      } else if (!m_mixed_mode_info.can_touch_coldstart_extended_set()) {
        always_assert_log(false,
                          "We shouldn't get here since we cleared "
                          "it up when emitting the mixed mode dex!\n");
      }
    }

    if (unreferenced_classes.count(cls)) {
      TRACE(IDEX, 3, "%s no longer linked to coldstart set.\n", SHOW(cls));
      cls_skipped_in_secondary++;
      continue;
    }

    emit_class(dex_info, cls, /* check_if_skip */ true,
               /* perf_sensitive */ true);
  }

  // Now emit the classes we omitted from the original coldstart set.
  for (DexType* type : interdex_types) {
    DexClass* cls = type_class(type);

    if (cls && unreferenced_classes.count(cls)) {
      emit_class(dex_info, cls, /* check_if_skip */ true,
                 /* perf_sensitive */ false);
    }
  }

  TRACE(IDEX, 3,
        "[interdex order]: %d classes are unreferenced from the interdex order "
        "in secondary dexes.\n",
        cls_skipped_in_secondary);
}

/**
 * Emit mix mode classes in a separate dex.
 * We respect the order of the classes in the interdexorder,
 * for the mixed mode classes that it contains.
 */
void InterDex::emit_mixed_mode_classes(
    const std::vector<DexType*>& interdex_types,
    bool can_touch_interdex_order) {
  always_assert_log(m_mixed_mode_info.has_predefined_classes(),
                    "No mixed mode classes to emit!\n");
  size_t pre_mixedmode_dexes = m_dexes_structure.get_num_dexes();

  DexInfo mixedmode_info;
  mixedmode_info.mixed_mode = true;

  // NOTE: When we got here, we would have removed the coldstart
  //       mixed mode classes, if we couldn't touch them.
  //       The only classes that might still be in the mixed_mode_cls
  //       set would be the extended ones, which we will remove
  //       if needed.
  for (DexType* type : interdex_types) {
    DexClass* clazz = type_class(type);

    if (m_mixed_mode_info.is_mixed_mode_class(clazz)) {
      if (can_touch_interdex_order) {
        TRACE(IDEX, 2,
              " Emitting mixed mode class, that is also in the "
              "interdex list: %s \n",
              SHOW(clazz));
        emit_class(mixedmode_info, clazz, /* check_if_skip */ false,
                   /* perf_sensitive */ true);
      }
      m_mixed_mode_info.remove_mixed_mode_class(clazz);
    }
  }

  for (const auto& clazz : m_mixed_mode_info.get_mixed_mode_classes()) {
    TRACE(IDEX, 2, " Emitting mixed mode class: %s \n", SHOW(clazz));
    emit_class(mixedmode_info, clazz, /* check_if_skip */ false,
               /* perf_sensitive */ true);
  }

  flush_out_dex(mixedmode_info);

  // NOTE: For now, we only support this to generate one dex.
  always_assert_log(m_dexes_structure.get_num_dexes() - pre_mixedmode_dexes ==
                        1,
                    "For now, we only support 1 dex for mixedmode classes.\n");

  // Clearing up the mixed mode classes.
  m_mixed_mode_info.remove_all_mixed_mode_classes();
}

namespace {

/**
 * Grab classes that should end up in a pre-defined interdex group.
 */
std::vector<std::vector<DexType*>> get_extra_classes_per_interdex_group(
    const Scope& scope) {
  std::vector<std::vector<DexType*>> res(MAX_DEX_NUM);

  size_t num_interdex_groups = 0;
  walk::classes(scope, [&](const DexClass* cls) {
    if (cls->rstate.has_interdex_subgroup()) {
      size_t interdex_subgroup = cls->rstate.get_interdex_subgroup();
      res[interdex_subgroup].push_back(cls->get_type());
      num_interdex_groups =
          std::max(num_interdex_groups, interdex_subgroup + 1);
    }
  });

  res.resize(num_interdex_groups);

  return res;
}

} // namespace

std::vector<DexType*> InterDex::get_interdex_types(const Scope& scope) {
  const std::vector<std::string>& interdexorder = m_cfg.get_coldstart_classes();

  // Find generated classes that should be in the interdex order.
  std::vector<std::vector<DexType*>> interdex_group_classes =
      get_extra_classes_per_interdex_group(scope);
  size_t curr_interdex_group = 0;

  std::unordered_set<DexClass*> classes(scope.begin(), scope.end());
  std::vector<DexType*> interdex_types;

  for (const auto& entry : interdexorder) {
    DexType* type = DexType::get_type(entry.c_str());
    if (!type) {
      if (entry.find(END_MARKER_FORMAT) != std::string::npos) {
        type = DexType::make_type(entry.c_str());
        m_end_markers.emplace_back(type);

        if (interdex_group_classes.size() > curr_interdex_group) {
          for (DexType* extra_type :
               interdex_group_classes.at(curr_interdex_group)) {
            interdex_types.emplace_back(extra_type);
          }
          curr_interdex_group++;
        }

        TRACE(IDEX, 4, "[interdex order]: Found class end marker %s.\n",
              entry.c_str());
      } else if (entry.find(SCROLL_MARKER_FORMAT) != std::string::npos) {
        type = DexType::make_type(entry.c_str());
        m_scroll_markers.emplace_back(type);
        TRACE(IDEX, 4, "[interdex order]: Found scroll class marker %s.\n",
              entry.c_str());
      } else {
        continue;
      }
    } else {
      DexClass* cls = type_class(type);
      if (!cls || classes.count(cls) == 0) {
        continue;
      }

      if (cls->rstate.has_interdex_subgroup()) {
        // Skipping generated classes that should end up in a specific group.
        continue;
      }
    }

    interdex_types.emplace_back(type);
  }

  // We still want to add the ones in the last interdex group, if any.
  always_assert_log(interdex_group_classes.size() <= curr_interdex_group + 2,
                    "Too many interdex subgroups!\n");
  if (interdex_group_classes.size() > curr_interdex_group) {
    for (DexType* type : interdex_group_classes.at(curr_interdex_group)) {
      interdex_types.push_back(type);
    }
  }

  return interdex_types;
}

void InterDex::update_interdexorder(const DexClasses& dex,
                                    std::vector<DexType*>* interdex_types) {
  // For all classes that are in the interdex order before
  // the first class end marker, we keep it at that position. Otherwise, we
  // add it to the head of the list.
  auto first_end_marker_it = interdex_types->end();
  auto last_end_marker_it = interdex_types->end();

  if (m_end_markers.size() == 0) {
    TRACE(IDEX, 3,
          "[coldstart classes]: Couldn't find any class end marker.\n");
  } else {
    first_end_marker_it = std::find(
        interdex_types->begin(), interdex_types->end(), m_end_markers.front());
    last_end_marker_it = std::find(interdex_types->begin(),
                                   interdex_types->end(), m_end_markers.back());
  }

  std::vector<DexType*> not_already_included;
  for (const auto& pclass : dex) {
    auto pclass_it = std::find(interdex_types->begin(), interdex_types->end(),
                               pclass->get_type());
    if (pclass_it == interdex_types->end() || pclass_it > first_end_marker_it) {
      TRACE(IDEX, 4, "Class %s is not in the interdex order.\n", SHOW(pclass));
      not_already_included.push_back(pclass->get_type());
    } else {
      TRACE(IDEX, 4, "Class %s is in the interdex order. No change required.\n",
            SHOW(pclass));
    }
  }
  interdex_types->insert(interdex_types->begin(),
                         not_already_included.begin(),
                         not_already_included.end());
}

void InterDex::init_cross_dex_ref_minimizer_and_relocate_methods(
    const Scope& scope) {
  TRACE(IDEX, 2,
        "[dex ordering] Cross-dex-ref-minimizer active with method ref weight "
        "%d, field ref weight %d, type ref weight %d, string ref weight %d, "
        "method seed weight %d, field seed weight %d, type seed weight %d, "
        "string seed weight %d.\n",
        m_cross_dex_ref_minimizer.get_config().method_ref_weight,
        m_cross_dex_ref_minimizer.get_config().field_ref_weight,
        m_cross_dex_ref_minimizer.get_config().type_ref_weight,
        m_cross_dex_ref_minimizer.get_config().string_ref_weight,
        m_cross_dex_ref_minimizer.get_config().method_seed_weight,
        m_cross_dex_ref_minimizer.get_config().field_seed_weight,
        m_cross_dex_ref_minimizer.get_config().type_seed_weight,
        m_cross_dex_ref_minimizer.get_config().string_seed_weight);

  if (m_cross_dex_relocator_config.relocate_static_methods ||
      m_cross_dex_relocator_config.relocate_non_static_direct_methods ||
      m_cross_dex_relocator_config.relocate_virtual_methods) {
    m_cross_dex_relocator = new CrossDexRelocator(
        m_cross_dex_relocator_config, m_original_scope, m_dexes_structure);

    TRACE(IDEX, 2,
          "[dex ordering] Cross-dex-relocator active, max relocated methods "
          "per class: %zu, relocating static methods: %s, non-static direct "
          "methods: %s, virtual methods: %s\n",
          m_cross_dex_relocator_config.max_relocated_methods_per_class,
          m_cross_dex_relocator_config.relocate_static_methods ? "yes" : "no",
          m_cross_dex_relocator_config.relocate_non_static_direct_methods
              ? "yes"
              : "no",
          m_cross_dex_relocator_config.relocate_virtual_methods ? "yes" : "no");
  }

  std::vector<DexClass*> classes_to_insert;
  // Emit classes using some algorithm to group together classes which
  // tend to share the same refs.
  for (DexClass* cls : scope) {
    // Don't bother with classes that emit_class will skip anyway.
    // (Postpone checking should_skip_class until after we have possibly
    // extracted relocatable methods.)
    if (is_canary(cls) || m_dexes_structure.has_class(cls)) {
      continue;
    }

    if (m_cross_dex_relocator != nullptr &&
        !should_not_relocate_methods_of_class(cls)) {
      std::vector<DexClass*> relocated_classes;
      m_cross_dex_relocator->relocate_methods(cls, relocated_classes);
      for (DexClass* relocated_cls : relocated_classes) {
        // Tell all plugins that the new class is now effectively part of the
        // scope.
        add_to_scope(relocated_cls);

        // It's important to call should_skip_class here, as some plugins
        // build up state for each class via this call.
        always_assert(!should_skip_class_due_to_plugin(relocated_cls));

        m_cross_dex_ref_minimizer.ignore(relocated_cls);
        classes_to_insert.emplace_back(relocated_cls);
      }
    }

    // Don't bother with classes that emit_class will skip anyway
    if (should_skip_class_due_to_plugin(cls)) {
      // Skipping a class due to a plugin might mean that (members of) of the
      // class will get emitted later via the additional-class mechanism,
      // which is accounted for via the erased_classes reported through the
      // plugin's gather_refs callback. So we'll also sample those classes here.
      m_cross_dex_ref_minimizer.sample(cls);
      continue;
    }

    if (should_skip_class_due_to_mixed_mode(EMPTY_DEX_INFO, cls)) {
      continue;
    }

    classes_to_insert.emplace_back(cls);
  }

  // Initialize ref frequency counts
  for (DexClass* cls : classes_to_insert) {
    m_cross_dex_ref_minimizer.sample(cls);
  }

  // Emit classes using some algorithm to group together classes which
  // tend to share the same refs.
  for (DexClass* cls : classes_to_insert) {
    m_cross_dex_ref_minimizer.insert(cls);
  }
}

void InterDex::emit_remaining_classes(const Scope& scope) {
  if (!m_minimize_cross_dex_refs) {
    for (DexClass* cls : scope) {
      emit_class(EMPTY_DEX_INFO, cls, /* check_if_skip */ true,
                 /* perf_sensitive */ false);
    }
    return;
  }

  init_cross_dex_ref_minimizer_and_relocate_methods(scope);

  int dexnum = m_dexes_structure.get_num_dexes();
  // Strategy for picking the next class to emit:
  // - at the beginning of a new dex, pick the "worst" class, i.e. the class
  //   with the most (adjusted) unapplied refs
  // - otherwise, pick the "best" class according to the priority scheme that
  //   prefers classes that share many applied refs and bring in few unapplied
  //   refs
  bool pick_worst = true;
  while (!m_cross_dex_ref_minimizer.empty()) {
    DexClass* cls = pick_worst ? m_cross_dex_ref_minimizer.worst()
                               : m_cross_dex_ref_minimizer.front();
    std::vector<DexClass*> erased_classes;
    bool emitted = emit_class(EMPTY_DEX_INFO, cls, /* check_if_skip */ false,
                              /* perf_sensitive */ false, &erased_classes);
    int new_dexnum = m_dexes_structure.get_num_dexes();
    bool overflowed = dexnum != new_dexnum;
    m_cross_dex_ref_minimizer.erase(cls, emitted, overflowed);

    if (m_cross_dex_relocator != nullptr) {
      // Let's merge relocated helper classes
      if (overflowed) {
        m_cross_dex_relocator->current_dex_overflowed();
      }
      m_cross_dex_relocator->add_to_current_dex(cls);
    }

    // We can treat *refs owned by "erased classes" as effectively being emitted
    for (DexClass* erased_cls : erased_classes) {
      TRACE(IDEX, 3, "[dex ordering] Applying erased class {%s}\n",
            SHOW(erased_cls));
      always_assert(should_skip_class_due_to_plugin(erased_cls));
      m_cross_dex_ref_minimizer.insert(erased_cls);
      m_cross_dex_ref_minimizer.erase(erased_cls, /* emitted */ true,
                                      /* overflowed */ false);
    }

    pick_worst = (pick_worst && !emitted) || overflowed;
    dexnum = new_dexnum;
  }
}

void InterDex::cleanup(const Scope& final_scope) {
  if (m_cross_dex_relocator != nullptr) {
    m_cross_dex_relocator->cleanup(final_scope);
  }
}

void InterDex::run() {
  auto scope = build_class_scope(m_dexen);

  std::vector<DexType*> interdex_types = get_interdex_types(scope);

  auto unreferenced_classes = find_unrefenced_coldstart_classes(
      scope, interdex_types, m_static_prune_classes);

  const auto& primary_dex = m_dexen[0];
  // We have a bunch of special logic for the primary dex which we only use if
  // we can't touch the primary dex.
  if (!m_normal_primary_dex) {
    emit_primary_dex(primary_dex, interdex_types, unreferenced_classes);
  }

  // NOTE: If primary dex is treated as a normal dex, we are going to modify
  //       it too, based on coldstart classes. Because of that, we need to
  //       update the coldstart list to respect the primary dex.
  if (m_normal_primary_dex && interdex_types.size() > 0) {
    update_interdexorder(primary_dex, &interdex_types);
  }

  // Emit interdex classes, if any.
  emit_interdex_classes(interdex_types, unreferenced_classes);

  // Now emit the classes that weren't specified in the head or primary list.
  emit_remaining_classes(scope);

  // Add whatever leftovers there are from plugins.
  for (const auto& plugin : m_plugins) {
    auto add_classes = plugin->leftover_classes();
    for (DexClass* add_class : add_classes) {
      TRACE(IDEX, 4, "IDEX: Emitting plugin generated leftover class :: %s\n",
            SHOW(add_class));
      emit_class(EMPTY_DEX_INFO, add_class, /* check_if_skip */ false,
                 /* perf_sensitive */ false);
    }
  }

  // Emit what is left, if any.
  if (m_dexes_structure.get_current_dex_classes().size()) {
    flush_out_dex(EMPTY_DEX_INFO);
  }

  always_assert_log(!m_emit_canaries ||
                        m_dexes_structure.get_num_dexes() < MAX_DEX_NUM,
                    "Bailing, max dex number surpassed %d\n",
                    m_dexes_structure.get_num_dexes());

  print_stats(&m_dexes_structure);
}

void InterDex::add_dexes_from_store(const DexStore& store) {
  const auto& dexes = store.get_dexen();
  for (const DexClasses& classes : dexes) {
    for (DexClass* cls : classes) {
      emit_class(EMPTY_DEX_INFO, cls, /* check_if_skip */ false,
                 /* perf_sensitive */ false);
    }
  }
  flush_out_dex(EMPTY_DEX_INFO);
}

/**
 * This needs to be called before getting to the next dex.
 */
void InterDex::flush_out_dex(DexInfo dex_info) {

  int dexnum = m_dexes_structure.get_num_dexes();
  if (dex_info.primary) {
    TRACE(IDEX, 2, "Writing out primary dex with %d classes.\n",
          m_dexes_structure.get_current_dex_classes().size());
  } else {
    TRACE(IDEX, 2,
          "Writing out secondary dex number %d, which is %s of coldstart, "
          "%s of extended set, %s scroll classes and has %d classes.\n",
          m_dexes_structure.get_num_secondary_dexes() + 1,
          (dex_info.coldstart ? "part of" : "not part of"),
          (dex_info.extended ? "part of" : "not part of"),
          (dex_info.scroll ? "has" : "doesn't have"),
          m_dexes_structure.get_current_dex_classes().size());
  }

  // Find the Canary class and add it in.
  if (m_emit_canaries && !dex_info.primary) {
    char buf[CANARY_CLASS_BUFSIZE];
    snprintf(buf, sizeof(buf), CANARY_CLASS_FORMAT, dexnum);
    std::string canary_name(buf);
    auto canary_type = DexType::get_type(canary_name);
    if (!canary_type) {
      TRACE(IDEX, 4, "Warning, no canary class %s found.\n", buf);
      canary_type = DexType::make_type(canary_name.c_str());
    }
    auto canary_cls = type_class(canary_type);
    if (!canary_cls) {
      ClassCreator cc(canary_type);
      cc.set_access(ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
      cc.set_super(get_object_type());
      canary_cls = cc.create();
    }
    m_dexes_structure.add_class_no_checks(canary_cls);

    // NOTE: We only emit this if we have canary classes.
    if (is_mixed_mode_dex(dex_info)) {
      dex_info.mixed_mode = true;

      always_assert_log(m_dexes_structure.get_num_mixedmode_dexes() == 0,
                        "For now, we only accept 1 mixed mode dex.\n");
      TRACE(IDEX, 2, "Secondary dex %d is considered for mixed mode\n",
            m_dexes_structure.get_num_secondary_dexes() + 1);

      auto mixed_mode_file = m_apk_manager.new_asset_file("mixed_mode.txt");
      auto mixed_mode_fh = FileHandle(*mixed_mode_file);
      mixed_mode_fh.seek_end();
      write_str(mixed_mode_fh, canary_name + "\n");
      *mixed_mode_file = nullptr;
    }
  }

  for (auto& plugin : m_plugins) {
    DexClasses classes = m_dexes_structure.get_current_dex_classes();
    const DexClasses& squashed_classes =
        m_dexes_structure.get_current_dex_squashed_classes();
    classes.insert(classes.end(), squashed_classes.begin(),
                   squashed_classes.end());
    auto add_classes = plugin->additional_classes(m_outdex, classes);
    for (auto add_class : add_classes) {
      TRACE(IDEX, 4, "IDEX: Emitting plugin-generated class :: %s\n",
            SHOW(add_class));
      m_dexes_structure.add_class_no_checks(add_class);
    }
  }

  m_outdex.emplace_back(m_dexes_structure.end_dex(dex_info));
}

bool InterDex::is_mixed_mode_dex(const DexInfo& dex_info) {
  if (dex_info.mixed_mode) {
    return true;
  }

  if (m_mixed_mode_info.has_status(DexStatus::FIRST_COLDSTART_DEX) &&
      m_dexes_structure.get_num_coldstart_dexes() == 0 && dex_info.coldstart) {
    return true;
  }

  if (m_mixed_mode_info.has_status(DexStatus::FIRST_EXTENDED_DEX) &&
      m_dexes_structure.get_num_extended_dexes() == 0 && dex_info.extended) {
    return true;
  }

  if (m_mixed_mode_info.has_status(DexStatus::SCROLL_DEX) &&
      m_dexes_structure.get_num_scroll_dexes() == 0 && dex_info.scroll) {
    return true;
  }

  return false;
}

} // namespace interdex
