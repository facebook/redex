/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDex.h"

#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <cinttypes>
#include <string>
#include <unordered_set>
#include <vector>

#include "CppUtil.h"
#include "Creators.h"
#include "Debug.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPassPlugin.h"
#include "MethodProfiles.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "StringUtil.h"
#include "Walkers.h"
#include "file-utils.h"

namespace {

constexpr const char* SECONDARY_CANARY_PREFIX = "Lsecondary/dex";
constexpr const char SECONDARY_CANARY_CLASS_FORMAT[] =
    "Lsecondary/dex%02d/Canary;";
constexpr size_t SECONDARY_CANARY_CLASS_BUFSIZE =
    sizeof(SECONDARY_CANARY_CLASS_FORMAT);

constexpr const char STORE_CANARY_CLASS_FORMAT[] = "Lstore%04x/dex%02d/Canary;";
constexpr size_t STORE_CANARY_CLASS_BUFSIZE = sizeof(STORE_CANARY_CLASS_FORMAT);

constexpr const char* END_MARKER_FORMAT = "LDexEndMarker";

constexpr const char* SCROLL_SET_START_FORMAT = "LScrollSetStart";
constexpr const char* SCROLL_SET_END_FORMAT = "LScrollSetEnd";

constexpr const char* BG_SET_START_FORMAT = "LBackgroundSetStart";
constexpr const char* BG_SET_END_FORMAT = "LBackgroundSetEnd";

static interdex::DexInfo EMPTY_DEX_INFO;

std::string get_canary_name(int dexnum, const DexString* store_name) {
  if (store_name) {
    char buf[STORE_CANARY_CLASS_BUFSIZE];
    int store_id = store_name->java_hashcode() & 0xFFFF;
    // Yes, there could be collisions. We assume that is handled outside of
    // Redex.
    snprintf(buf, sizeof(buf), STORE_CANARY_CLASS_FORMAT, store_id, dexnum + 1);
    return std::string(buf);
  } else {
    char buf[SECONDARY_CANARY_CLASS_BUFSIZE];
    snprintf(buf, sizeof(buf), SECONDARY_CANARY_CLASS_FORMAT, dexnum);
    return std::string(buf);
  }
}

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
    walk::code(
        input_scope,
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
    TRACE(IDEX, 4, "Found %d classes in coldstart with no references.",
          new_no_ref);
    input_scope = output_scope;
  }

  return unreferenced_classes;
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
  TRACE(IDEX, 2, "InterDex Stats:");
  TRACE(IDEX, 2, "\t dex count: %zu", dexes_structure->get_num_dexes());
  TRACE(IDEX, 2, "\t secondary dex count: %zu",
        dexes_structure->get_num_secondary_dexes());
  TRACE(IDEX, 2, "\t coldstart dex count: %zu",
        dexes_structure->get_num_coldstart_dexes());
  TRACE(IDEX, 2, "\t extendex dex count: %zu",
        dexes_structure->get_num_extended_dexes());
  TRACE(IDEX, 2, "\t scroll dex count: %zu",
        dexes_structure->get_num_scroll_dexes());

  TRACE(IDEX, 2, "Global stats:");
  TRACE(IDEX, 2, "\t %lu classes", dexes_structure->get_num_classes());
  TRACE(IDEX, 2, "\t %lu mrefs", dexes_structure->get_num_mrefs());
  TRACE(IDEX, 2, "\t %lu frefs", dexes_structure->get_num_frefs());
  TRACE(IDEX, 2, "\t %lu dmethods", dexes_structure->get_num_dmethods());
  TRACE(IDEX, 2, "\t %lu vmethods", dexes_structure->get_num_vmethods());
  TRACE(IDEX, 2, "\t %lu mrefs", dexes_structure->get_num_mrefs());
}

/**
 * Order the classes in `scope` according to the`coldstart_class_names`.
 */
void do_order_classes(const std::vector<std::string>& coldstart_class_names,
                      Scope* scope) {
  std::unordered_map<const DexClass*, uint32_t> class_to_priority;
  uint32_t priority = 0;
  for (const auto& class_name : coldstart_class_names) {
    if (DexType* type = DexType::get_type(class_name.c_str())) {
      if (auto cls = type_class(type)) {
        class_to_priority[cls] = priority++;
        cls->set_perf_sensitive(true);
      }
    }
  }
  TRACE(IDEX, 3, "IDEX: Ordered around %d classes at the beginning", priority);
  std::stable_sort(
      scope->begin(), scope->end(),
      [&class_to_priority](const DexClass* left, const DexClass* right) {
        uint32_t left_priority = std::numeric_limits<uint32_t>::max();
        uint32_t right_priority = std::numeric_limits<uint32_t>::max();
        auto it = class_to_priority.find(left);
        if (it != class_to_priority.end()) {
          left_priority = it->second;
        }
        it = class_to_priority.find(right);
        if (it != class_to_priority.end()) {
          right_priority = it->second;
        }
        return left_priority < right_priority;
      });
}

} // namespace

namespace interdex {

bool is_canary(DexClass* clazz) {
  const char* cname = clazz->get_type()->get_name()->c_str();
  return strncmp(cname, SECONDARY_CANARY_PREFIX,
                 strlen(SECONDARY_CANARY_PREFIX)) == 0;
}

// Compare two classes for sorting in a way that is best for compression.
bool compare_dexclasses_for_compressed_size(DexClass* c1, DexClass* c2) {
  // Canary classes go last
  if (interdex::is_canary(c1) != interdex::is_canary(c2)) {
    return (interdex::is_canary(c1) ? 1 : 0) <
           (interdex::is_canary(c2) ? 1 : 0);
  }
  // Interfaces go after non-interfaces
  if (is_interface(c1) != is_interface(c2)) {
    return (is_interface(c1) ? 1 : 0) < (is_interface(c2) ? 1 : 0);
  }
  // Base types and implemented interfaces go last
  if (type::check_cast(c2->get_type(), c1->get_type())) {
    return false;
  }
  always_assert(c1 != c2);
  if (type::check_cast(c1->get_type(), c2->get_type())) {
    return true;
  }
  // If types are unrelated, sort by super-classes and then
  // interfaces
  if (c1->get_super_class() != c2->get_super_class()) {
    return compare_dextypes(c1->get_super_class(), c2->get_super_class());
  }
  if (c1->get_interfaces() != c2->get_interfaces()) {
    return compare_dextypelists(c1->get_interfaces(), c2->get_interfaces());
  }

  // Tie-breaker: fields/methods count distance
  int dmethods_distance =
      (int)c1->get_dmethods().size() - (int)c2->get_dmethods().size();
  if (dmethods_distance != 0) {
    return dmethods_distance < 0;
  }
  int vmethods_distance =
      (int)c1->get_vmethods().size() - (int)c2->get_vmethods().size();
  if (vmethods_distance != 0) {
    return vmethods_distance < 0;
  }
  int ifields_distance =
      (int)c1->get_ifields().size() - (int)c2->get_ifields().size();
  if (ifields_distance != 0) {
    return ifields_distance < 0;
  }
  int sfields_distance =
      (int)c1->get_sfields().size() - (int)c2->get_sfields().size();
  if (sfields_distance != 0) {
    return sfields_distance < 0;
  }
  // Tie-breaker: has-class-data
  if (c1->has_class_data() != c2->has_class_data()) {
    return (c1->has_class_data() ? 1 : 0) < (c2->has_class_data() ? 1 : 0);
  }
  // Final tie-breaker: Compare types, which means names
  return compare_dextypes(c1->get_type(), c2->get_type());
}

bool InterDex::should_skip_class_due_to_plugin(DexClass* clazz) {
  for (const auto& plugin : m_plugins) {
    if (plugin->should_skip_class(clazz)) {
      TRACE(IDEX, 4, "IDEX: Skipping class from %s :: %s",
            plugin->name().c_str(), SHOW(clazz));
      return true;
    }
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
      TRACE(IDEX, 4, "IDEX: Not relocating methods of class from %s :: %s",
            plugin->name().c_str(), SHOW(clazz));
      return true;
    }
  }

  return false;
}

InterDex::EmitResult InterDex::emit_class(
    DexInfo& dex_info,
    DexClass* clazz,
    bool check_if_skip,
    bool perf_sensitive,
    DexClass** canary_cls,
    std::vector<DexClass*>* erased_classes) {
  if (is_canary(clazz)) {
    // Nothing to do here.
    return {false, false};
  }

  if (m_dexes_structure.has_class(clazz)) {
    TRACE(IDEX, 6, "Trying to re-add class %s!", SHOW(clazz));
    return {false, false};
  }

  if (check_if_skip && (should_skip_class_due_to_plugin(clazz))) {
    return {false, false};
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
    flush_out_dex(dex_info, *canary_cls);
    *canary_cls = get_canary_cls(dex_info);

    // Plugins may maintain internal state after gathering refs, and
    // then they tend to forget that state after flushing out (class
    // merging, looking at you). So, let's redo gathering of refs here
    // to give plugins a chance to rebuild their internal state.
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
  return {true, !fits_current_dex};
}

void InterDex::emit_primary_dex(
    const DexClasses& primary_dex,
    const std::vector<DexType*>& interdex_order,
    const std::unordered_set<DexClass*>& unreferenced_classes) {

  std::unordered_set<DexClass*> primary_dex_set(primary_dex.begin(),
                                                primary_dex.end());

  DexInfo primary_dex_info;
  primary_dex_info.primary = true;

  size_t coldstart_classes_in_primary = 0;
  size_t coldstart_classes_skipped_in_primary = 0;

  // Sort the primary dex according to interdex order (aka emit first the
  // primary classes that appear in the interdex order, in the order that
  // they appear there).
  for (DexType* type : interdex_order) {
    DexClass* cls = type_class(type);
    if (!cls) {
      continue;
    }

    if (primary_dex_set.count(cls) > 0) {
      if (unreferenced_classes.count(cls) > 0) {
        TRACE(IDEX, 5, "[primary dex]: %s no longer linked to coldstart set.",
              SHOW(cls));
        coldstart_classes_skipped_in_primary++;
        continue;
      }

      emit_class(primary_dex_info, cls, /* check_if_skip */ true,
                 /* perf_sensitive */ true, /* canary_cls */ nullptr);
      coldstart_classes_in_primary++;
    }
  }

  // Now add the rest.
  for (DexClass* cls : primary_dex) {
    emit_class(primary_dex_info, cls, /* check_if_skip */ true,
               /* perf_sensitive */ false, /* canary_cls */ nullptr);
  }
  TRACE(IDEX, 2,
        "[primary dex]: %zu out of %zu classes in primary dex "
        "from interdex list.",
        coldstart_classes_in_primary, primary_dex.size());
  TRACE(IDEX, 2,
        "[primary dex]: %zu out of %zu classes in primary dex skipped "
        "from interdex list.",
        coldstart_classes_skipped_in_primary, primary_dex.size());

  flush_out_dex(primary_dex_info, /* canary_cls */ nullptr);

  // Double check only 1 dex was created.
  always_assert_log(
      m_dexes_structure.get_num_dexes() == 1,
      "[error]: Primary dex doesn't fit in only 1 dex anymore :|, but in %zu\n",
      m_dexes_structure.get_num_dexes());
}

void InterDex::emit_interdex_classes(
    DexInfo& dex_info,
    const std::vector<DexType*>& interdex_types,
    const std::unordered_set<DexClass*>& unreferenced_classes,
    DexClass** canary_cls) {
  if (interdex_types.empty()) {
    TRACE(IDEX, 2, "No interdex classes passed.");
    return;
  }

  // NOTE: coldstart has no interaction with extended and scroll set, but that
  //       is not true for the later 2.
  dex_info.coldstart = true;

  size_t cls_skipped_in_secondary = 0;

  bool reset_coldstart_on_overflow = false;
  for (auto it = interdex_types.begin(); it != interdex_types.end(); ++it) {
    DexType* type = *it;
    DexClass* cls = type_class(type);
    if (!cls) {
      TRACE(IDEX, 5, "[interdex classes]: No such entry %s.", SHOW(type));
      if (boost::algorithm::starts_with(type->get_name()->str(),
                                        SCROLL_SET_START_FORMAT)) {
        always_assert_log(
            !m_emitting_scroll_set,
            "Scroll start marker discovered after another scroll start marker");
        always_assert_log(
            !m_emitting_bg_set,
            "Scroll start marker discovered between background set markers");
        m_emitting_scroll_set = true;
        TRACE(IDEX, 2, "Marking dex as scroll at betamap entry %zu",
              std::distance(interdex_types.begin(), it));
        dex_info.scroll = true;
      } else if (boost::algorithm::starts_with(type->get_name()->str(),
                                               SCROLL_SET_END_FORMAT)) {
        always_assert_log(
            m_emitting_scroll_set,
            "Scroll end marker discovered without scroll start marker");
        m_emitting_scroll_set = false;
      } else if (boost::algorithm::starts_with(type->get_name()->str(),
                                               BG_SET_START_FORMAT)) {
        always_assert_log(!m_emitting_bg_set,
                          "Background start marker discovered after another "
                          "background start marker");
        always_assert_log(
            !m_emitting_scroll_set,
            "Background start marker discovered between scroll set markers");
        TRACE(IDEX, 2, "Marking dex as background at betamap entry %zu",
              std::distance(interdex_types.begin(), it));
        m_emitting_bg_set = true;
        dex_info.background = true;
      } else if (boost::algorithm::starts_with(type->get_name()->str(),
                                               BG_SET_END_FORMAT)) {
        always_assert_log(
            m_emitting_bg_set,
            "Background end marker discovered without background start marker");
        m_emitting_bg_set = false;
        m_emitted_bg_set = true;
      } else {
        auto end_marker =
            std::find(m_end_markers.begin(), m_end_markers.end(), type);
        // Cold start end marker is the last dex end marker
        auto cold_start_end_marker = !m_end_markers.empty()
                                         ? m_end_markers.end() - 1
                                         : m_end_markers.end();
        if (end_marker != m_end_markers.end()) {
          always_assert_log(
              !m_emitting_scroll_set,
              "End marker discovered between scroll start/end markers");
          always_assert_log(
              !m_emitting_bg_set,
              "End marker discovered between background start/end markers");
          TRACE(IDEX, 2, "Terminating dex due to %s", SHOW(type));
          if (end_marker != cold_start_end_marker ||
              !m_fill_last_coldstart_dex || m_end_markers.size() == 1) {
            flush_out_dex(dex_info, *canary_cls);
            *canary_cls = get_canary_cls(dex_info);
            if (end_marker == cold_start_end_marker) {
              dex_info.coldstart = false;
            }
          } else {
            // if (end_marker == cold_start_end_marker) {
            //   dex_info.coldstart = false;
            reset_coldstart_on_overflow = true;
          }
        }
      }
    } else {
      if (unreferenced_classes.count(cls)) {
        TRACE(IDEX, 3, "%s no longer linked to coldstart set.", SHOW(cls));
        cls_skipped_in_secondary++;
        continue;
      }
      if (m_emitted_bg_set) {
        m_emitted_bg_set = false;
        dex_info.extended = true;
        m_emitting_extended = true;
      }
      dex_info.betamap_ordered = true;
      auto res = emit_class(dex_info, cls, /* check_if_skip */ true,
                            /* perf_sensitive */ true, canary_cls);

      if (res.overflowed && reset_coldstart_on_overflow) {
        dex_info.coldstart = false;
        reset_coldstart_on_overflow = false;
      }
    }
  }

  // Now emit the classes we omitted from the original coldstart set.
  for (DexType* type : interdex_types) {
    DexClass* cls = type_class(type);

    if (cls && unreferenced_classes.count(cls)) {
      auto res = emit_class(dex_info, cls, /* check_if_skip */ true,
                            /* perf_sensitive */ false, canary_cls);

      if (res.overflowed && reset_coldstart_on_overflow) {
        dex_info.coldstart = false;
        reset_coldstart_on_overflow = false;
      }
    }
  }

  TRACE(IDEX, 3,
        "[interdex order]: %zu classes are unreferenced from the interdex "
        "order in secondary dexes.",
        cls_skipped_in_secondary);

  // TODO: check for unterminated markers
  always_assert_log(!m_emitting_scroll_set, "Unterminated scroll set marker");
  always_assert_log(!m_emitting_bg_set, "Unterminated background set marker");

  m_emitting_extended = false;
}

namespace {

/**
 * Grab classes that should end up in a pre-defined interdex group.
 */
std::vector<std::vector<DexType*>> get_extra_classes_per_interdex_group(
    const Scope& scope) {
  std::vector<std::vector<DexType*>> res(MAX_DEX_NUM);

  InterdexSubgroupIdx num_interdex_groups = 0;
  walk::classes(scope, [&](const DexClass* cls) {
    if (cls->rstate.has_interdex_subgroup()) {
      InterdexSubgroupIdx interdex_subgroup =
          cls->rstate.get_interdex_subgroup();
      res[interdex_subgroup].push_back(cls->get_type());
      num_interdex_groups =
          std::max(num_interdex_groups, interdex_subgroup + 1);
    }
  });

  res.resize(num_interdex_groups);

  return res;
}

} // namespace

void InterDex::load_interdex_types() {
  always_assert(m_interdex_types.empty());

  const std::vector<std::string>& interdexorder =
      m_conf.get_coldstart_classes();

  // Find generated classes that should be in the interdex order.
  std::vector<std::vector<DexType*>> interdex_group_classes =
      get_extra_classes_per_interdex_group(m_scope);
  size_t curr_interdex_group = 0;

  std::unordered_set<DexClass*> classes(m_scope.begin(), m_scope.end());

  std::unordered_set<DexType*> all_set{};

  if (m_transitively_close_interdex_order && !m_force_single_dex) {
    for (auto* cls : m_dexen[0]) {
      all_set.insert(cls->get_type()); // Ignore primary.
    }
  }

  std::unordered_set<DexType*> moved_or_double{};
  std::unordered_set<DexType*> transitive_added{};

  for (const auto& entry : interdexorder) {
    DexType* type = DexType::get_type(entry.c_str());
    if (!type) {
      if (boost::algorithm::starts_with(entry, END_MARKER_FORMAT)) {
        type = DexType::make_type(entry.c_str());
        m_end_markers.emplace_back(type);

        if (interdex_group_classes.size() > curr_interdex_group) {
          for (DexType* extra_type :
               interdex_group_classes.at(curr_interdex_group)) {
            m_interdex_types.emplace_back(extra_type);
          }
          curr_interdex_group++;
        }

        TRACE(IDEX, 4, "[interdex order]: Found class end marker %s.",
              entry.c_str());
      } else if (boost::algorithm::starts_with(entry,
                                               SCROLL_SET_START_FORMAT)) {
        type = DexType::make_type(entry.c_str());
        TRACE(IDEX, 4,
              "[interdex order]: Found scroll set start class marker %s.",
              entry.c_str());
      } else if (boost::algorithm::starts_with(entry, SCROLL_SET_END_FORMAT)) {
        type = DexType::make_type(entry.c_str());
        TRACE(IDEX, 4,
              "[interdex order]: Found scroll set end class marker %s.",
              entry.c_str());
      } else if (boost::algorithm::starts_with(entry, BG_SET_START_FORMAT)) {
        type = DexType::make_type(entry.c_str());
        TRACE(IDEX, 4, "[interdex order]: Found bg set start class marker %s.",
              entry.c_str());
      } else if (boost::algorithm::starts_with(entry, BG_SET_END_FORMAT)) {
        type = DexType::make_type(entry.c_str());
        TRACE(IDEX, 4, "[interdex order]: Found bg set end class marker %s.",
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

      if (m_transitively_close_interdex_order) {
        if (all_set.count(type) != 0) {
          // Moved earlier.
          moved_or_double.insert(type);
          continue;
        }

        // Transitive closure.
        self_recursive_fn(
            [&](const auto& self, DexType* cur, bool add_self) {
              DexClass* cur_cls = type_class(cur);
              if (!cur_cls || classes.count(cur_cls) == 0 ||
                  all_set.count(cur) != 0) {
                return;
              }
              all_set.insert(cur);
              if (add_self) {
                transitive_added.insert(cur);
              }

              // Superclass first.
              self(self, cur_cls->get_super_class(), true);
              // Then interfaces.
              for (auto* intf : *cur_cls->get_interfaces()) {
                self(self, intf, true);
              }

              // Then self.
              if (add_self) {
                m_interdex_types.emplace_back(cur);
              }
            },
            type, false);
      }
    }

    m_interdex_types.emplace_back(type);
  }

  // We still want to add the ones in the last interdex group, if any.
  always_assert_log(interdex_group_classes.size() <= curr_interdex_group + 2,
                    "Too many interdex subgroups!\n");
  if (interdex_group_classes.size() > curr_interdex_group) {
    for (DexType* type : interdex_group_classes.at(curr_interdex_group)) {
      // TODO: Does the above need to filter? Do we need to transitively close
      //       here, too?
      if (!m_transitively_close_interdex_order || all_set.count(type) == 0) {
        m_interdex_types.push_back(type);
      }
    }
  }

  if (m_transitively_close_interdex_order) {
    std::unordered_set<DexType*> transitive_moved;
    for (auto* t : moved_or_double) {
      if (transitive_added.count(t) != 0) {
        transitive_moved.insert(t);
        transitive_added.erase(t);
      }
    }

    m_transitive_closure_added = transitive_added.size();
    m_transitive_closure_moved = transitive_moved.size();
  }
}

void InterDex::update_interdexorder(const DexClasses& dex,
                                    std::vector<DexType*>* interdex_types) {
  std::vector<DexType*> primary_dex;
  for (DexClass* cls : dex) {
    primary_dex.emplace_back(cls->get_type());
  }

  // We keep the primary classes untouched - at the beginning of
  // the interdex list.
  interdex_types->insert(interdex_types->begin(), primary_dex.begin(),
                         primary_dex.end());
}

void InterDex::init_cross_dex_ref_minimizer_and_relocate_methods() {
  TRACE(IDEX, 2,
        "[dex ordering] Cross-dex-ref-minimizer active with method ref weight "
        "%" PRIu64 ", field ref weight %" PRIu64 ", type ref weight %" PRIu64
        ", string ref weight %" PRIu64 ", method seed weight %" PRIu64
        ", field seed weight %" PRIu64 ", type seed weight %" PRIu64
        ", string seed weight %" PRIu64 ".",
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
    m_cross_dex_relocator =
        new CrossDexRelocator(m_cross_dex_relocator_config, m_original_scope,
                              m_xstore_refs, m_dexes_structure);

    TRACE(IDEX, 2,
          "[dex ordering] Cross-dex-relocator active, max relocated methods "
          "per class: %" PRIu64
          ", relocating static methods: %s"
          ", non-static direct methods: %s"
          ", virtual methods: %s",
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
  for (DexClass* cls : m_scope) {
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

  // A few classes might have already been emitted to the current dex which we
  // are about to fill up. Make it so that the minimizer knows that all the refs
  // of those classes have already been emitted.
  for (auto cls : m_dexes_structure.get_current_dex_classes()) {
    m_cross_dex_ref_minimizer.sample(cls);
    m_cross_dex_ref_minimizer.insert(cls);
    m_cross_dex_ref_minimizer.erase(cls, /* emitted */ true,
                                    /* overflowed */ false);
  }
}

void InterDex::emit_remaining_classes(DexInfo& dex_info,
                                      DexClass** canary_cls) {
  m_current_classes_when_emitting_remaining =
      m_dexes_structure.get_current_dex_classes().size();

  if (!m_minimize_cross_dex_refs) {
    for (DexClass* cls : m_scope) {
      emit_class(dex_info, cls, /* check_if_skip */ true,
                 /* perf_sensitive */ false, canary_cls);
    }
    return;
  }

  init_cross_dex_ref_minimizer_and_relocate_methods();

  int dexnum = m_dexes_structure.get_num_dexes();
  // Strategy for picking the next class to emit:
  // - at the beginning of a new dex, pick the "worst" class, i.e. the class
  //   with the most (adjusted) unapplied refs
  // - otherwise, pick the "best" class according to the priority scheme that
  //   prefers classes that share many applied refs and bring in few unapplied
  //   refs
  bool pick_worst = true;
  while (!m_cross_dex_ref_minimizer.empty()) {
    DexClass* cls{nullptr};
    if (pick_worst) {
      // Figure out which class has the most unapplied references
      auto worst = m_cross_dex_ref_minimizer.worst();
      // Use that worst class if it has more unapplied refs than already applied
      // refs
      if (m_cross_dex_ref_minimizer.get_unapplied_refs(worst) >
          m_cross_dex_ref_minimizer.get_applied_refs()) {
        cls = worst;
      }
    }
    if (!cls) {
      // Default case
      cls = m_cross_dex_ref_minimizer.front();
    }

    std::vector<DexClass*> erased_classes;
    bool emitted =
        emit_class(dex_info, cls, /* check_if_skip */ false,
                   /* perf_sensitive */ false, canary_cls, &erased_classes);
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
      TRACE(IDEX, 3, "[dex ordering] Applying erased class {%s}",
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

void InterDex::run_in_force_single_dex_mode() {
  auto scope = build_class_scope(m_dexen);

  const std::vector<std::string>& coldstart_class_names =
      m_conf.get_coldstart_classes();
  DexInfo dex_info;
  dex_info.primary = true;
  if (coldstart_class_names.empty()) {
    TRACE(IDEX, 3, "IDEX single dex mode: No coldstart_classes");
  } else {
    dex_info.coldstart = true;
    do_order_classes(coldstart_class_names, &scope);
  }

  // Add all classes into m_dexes_structure without further checking when
  // force_single_dex is on. The overflow checking will be done later on at
  // the end of the pipeline (e.g. write_classes_to_dex).
  for (DexClass* cls : scope) {
    MethodRefs clazz_mrefs;
    FieldRefs clazz_frefs;
    TypeRefs clazz_trefs;
    std::vector<DexClass*> erased_classes;

    gather_refs(m_plugins, dex_info, cls, &clazz_mrefs, &clazz_frefs,
                &clazz_trefs, &erased_classes,
                should_not_relocate_methods_of_class(cls));

    m_dexes_structure.add_class_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                                          cls);
  }

  // Emit all no matter what it is.
  if (!m_dexes_structure.get_current_dex_classes().empty()) {
    flush_out_dex(dex_info, /* canary_cls */ nullptr);
  }

  TRACE(IDEX, 7, "IDEX: force_single_dex dex number: %zu",
        m_dexes_structure.get_num_dexes());
  print_stats(&m_dexes_structure);
}

void InterDex::run() {
  TRACE(IDEX, 2, "IDEX: Running on root store");
  if (m_force_single_dex) {
    run_in_force_single_dex_mode();
    return;
  }

  auto unreferenced_classes = find_unrefenced_coldstart_classes(
      m_scope, m_interdex_types, m_static_prune_classes);

  const auto& primary_dex = m_dexen[0];
  // We have a bunch of special logic for the primary dex which we only use if
  // we can't touch the primary dex.
  if (!m_normal_primary_dex) {
    emit_primary_dex(primary_dex, m_interdex_types, unreferenced_classes);
  } else {
    // NOTE: If primary dex is treated as a normal dex, we are going to modify
    //       it too, based on coldstart classes. If we can't remove the classes
    //       from the primary dex, we need to update the coldstart list to
    //       respect the primary dex.
    if (m_keep_primary_order && !m_interdex_types.empty()) {
      update_interdexorder(primary_dex, &m_interdex_types);
    }
  }

  // Emit interdex classes, if any.
  DexInfo dex_info;
  DexClass* canary_cls = get_canary_cls(dex_info);
  emit_interdex_classes(dex_info, m_interdex_types, unreferenced_classes,
                        &canary_cls);

  // Now emit the classes that weren't specified in the head or primary list.
  emit_remaining_classes(dex_info, &canary_cls);

  // Add whatever leftovers there are from plugins.
  for (const auto& plugin : m_plugins) {
    auto add_classes = plugin->leftover_classes();
    std::string name = plugin->name();
    for (DexClass* add_class : add_classes) {
      TRACE(IDEX, 4, "IDEX: Emitting %s-plugin generated leftover class :: %s",
            name.c_str(), SHOW(add_class));
      emit_class(dex_info, add_class, /* check_if_skip */ false,
                 /* perf_sensitive */ false, &canary_cls);
    }
  }

  // Emit what is left, if any.
  if (!m_dexes_structure.get_current_dex_classes().empty()) {
    flush_out_dex(dex_info, canary_cls);
    canary_cls = nullptr;
  }

  // Emit dex info manifest
  if (m_asset_manager.has_secondary_dex_dir()) {
    auto mixed_mode_file = m_asset_manager.new_asset_file("dex_manifest.txt");
    auto mixed_mode_fh = FileHandle(*mixed_mode_file);
    mixed_mode_fh.seek_end();
    std::stringstream ss;
    int ordinal = 0;
    for (const auto& info : m_dex_infos) {
      const auto& flags = std::get<1>(info);
      ss << std::get<0>(info) << ",ordinal=" << ordinal++
         << ",coldstart=" << flags.coldstart << ",extended=" << flags.extended
         << ",primary=" << flags.primary << ",scroll=" << flags.scroll
         << ",background=" << flags.background << std::endl;
    }
    write_str(mixed_mode_fh, ss.str());
    *mixed_mode_file = nullptr;
  }

  always_assert_log(!m_emit_canaries ||
                        m_dexes_structure.get_num_dexes() < MAX_DEX_NUM,
                    "Bailing, max dex number surpassed %zu\n",
                    m_dexes_structure.get_num_dexes());

  print_stats(&m_dexes_structure);
}

void InterDex::run_on_nonroot_store() {
  TRACE(IDEX, 2, "IDEX: Running on non-root store");
  auto canary_cls = get_canary_cls(EMPTY_DEX_INFO);
  for (DexClass* cls : m_scope) {
    emit_class(EMPTY_DEX_INFO, cls, /* check_if_skip */ false,
               /* perf_sensitive */ false, &canary_cls);
  }

  // Emit what is left, if any.
  if (!m_dexes_structure.get_current_dex_classes().empty()) {
    flush_out_dex(EMPTY_DEX_INFO, canary_cls);
  }

  print_stats(&m_dexes_structure);
}

void InterDex::add_dexes_from_store(const DexStore& store) {
  auto canary_cls = get_canary_cls(EMPTY_DEX_INFO);
  const auto& dexes = store.get_dexen();
  for (const DexClasses& classes : dexes) {
    for (DexClass* cls : classes) {
      emit_class(EMPTY_DEX_INFO, cls, /* check_if_skip */ false,
                 /* perf_sensitive */ false, &canary_cls);
    }
  }
  flush_out_dex(EMPTY_DEX_INFO, canary_cls);
}

void InterDex::set_clinit_methods_if_needed(DexClass* cls) {
  using namespace dex_asm;

  if (m_methods_for_canary_clinit_reference.empty()) {
    // No methods to call from clinit; don't create clinit.
    return;
  }

  // Create a clinit static method.
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  DexMethod* clinit =
      DexMethod::make_method(cls->get_type(),
                             DexString::make_string("<clinit>"), proto)
          ->make_concrete(ACC_STATIC | ACC_CONSTRUCTOR, false);
  clinit->set_code(std::make_unique<IRCode>());
  cls->add_method(clinit);
  clinit->set_deobfuscated_name(show_deobfuscated(clinit));

  // Add code to clinit to call the other methods.
  auto code = clinit->get_code();
  size_t max_size = 0;
  for (const auto& method_name : m_methods_for_canary_clinit_reference) {
    // No need to do anything if this method isn't present in the build.
    if (DexMethodRef* method = DexMethod::get_method(method_name)) {
      std::vector<Operand> reg_operands;
      int64_t reg = 0;
      for (auto* dex_type : *method->get_proto()->get_args()) {
        Operand reg_operand = {VREG, reg};
        switch (dex_type->get_name()->c_str()[0]) {
        case 'J':
        case 'D':
          // 8 bytes
          code->push_back(dasm(OPCODE_CONST_WIDE, {reg_operand, 0_L}));
          reg_operands.push_back(reg_operand);
          reg += 2;
          break;
        default:
          // 4 or fewer bytes
          code->push_back(dasm(OPCODE_CONST, {reg_operand, 0_L}));
          reg_operands.push_back(reg_operand);
          ++reg;
          break;
        }
      }
      max_size = std::max(max_size, (size_t)reg);
      code->push_back(dasm(OPCODE_INVOKE_STATIC, method, reg_operands.begin(),
                           reg_operands.end()));
    }
  }
  code->set_registers_size(max_size);
  code->push_back(dasm(OPCODE_RETURN_VOID));
}

DexClass* create_canary(int dexnum, const DexString* store_name) {
  std::string canary_name = get_canary_name(dexnum, store_name);
  auto canary_type = DexType::get_type(canary_name);
  if (!canary_type) {
    TRACE(IDEX, 4, "Warning, no canary class %s found.", canary_name.c_str());
    canary_type = DexType::make_type(canary_name.c_str());
  }
  auto canary_cls = type_class(canary_type);
  if (!canary_cls) {
    ClassCreator cc(canary_type);
    cc.set_access(ACC_PUBLIC | ACC_ABSTRACT);
    cc.set_super(type::java_lang_Object());
    canary_cls = cc.create();
    // Don't rename the Canary we've created
    canary_cls->rstate.set_keepnames();
  }
  return canary_cls;
}

// Creates a canary class if necessary. (In particular, the primary dex never
// has a canary class.) This method should be called after flush_out_dex when
// beginning a new dex. As canary classes are added in the end without checks,
// the implied references are added here immediately to ensure that we don't
// exceed limits.
DexClass* InterDex::get_canary_cls(DexInfo& dex_info) {
  if (!m_emit_canaries || dex_info.primary) {
    return nullptr;
  }
  int dexnum = m_dexes_structure.get_num_dexes();
  auto canary_cls = create_canary(dexnum);
  set_clinit_methods_if_needed(canary_cls);
  MethodRefs clazz_mrefs;
  FieldRefs clazz_frefs;
  TypeRefs clazz_trefs;
  canary_cls->gather_methods(clazz_mrefs);
  canary_cls->gather_fields(clazz_frefs);
  canary_cls->gather_types(clazz_trefs);
  m_dexes_structure.add_refs_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs);
  return canary_cls;
}

/**
 * This needs to be called before getting to the next dex.
 */
void InterDex::flush_out_dex(DexInfo& dex_info, DexClass* canary_cls) {

  int dexnum = m_dexes_structure.get_num_dexes();
  if (dex_info.primary) {
    TRACE(IDEX, 2, "Writing out primary dex with %zu classes.",
          m_dexes_structure.get_current_dex_classes().size());
  } else {
    TRACE(IDEX, 2,
          "Writing out secondary dex number %zu, which is %s of coldstart, "
          "%s of extended set, %s of background set, %s scroll "
          "classes and has %zu classes.",
          m_dexes_structure.get_num_secondary_dexes() + 1,
          (dex_info.coldstart ? "part of" : "not part of"),
          (dex_info.extended ? "part of" : "not part of"),
          (dex_info.background ? "part of" : "not part of"),
          (dex_info.scroll ? "has" : "doesn't have"),
          m_dexes_structure.get_current_dex_classes().size());
  }

  // Add the Canary class, if any.
  if (canary_cls) {
    always_assert(
        m_dexes_structure.current_dex_has_tref(canary_cls->get_type()));


    // Properly try to insert the class.

    MethodRefs clazz_mrefs;
    FieldRefs clazz_frefs;
    TypeRefs clazz_trefs;
    std::vector<DexClass*> erased_classes;
    gather_refs(m_plugins, dex_info, canary_cls, &clazz_mrefs, &clazz_frefs,
                &clazz_trefs, &erased_classes, true);

    bool canary_added = m_dexes_structure.add_class_to_current_dex(
        clazz_mrefs, clazz_frefs, clazz_trefs, canary_cls);
    always_assert(canary_added);

    m_dex_infos.emplace_back(
        std::make_tuple(canary_cls->get_name()->str(), dex_info));
  }

  std::unordered_set<DexClass*> additional_classes;
  for (auto& plugin : m_plugins) {
    DexClasses classes = m_dexes_structure.get_current_dex_classes();
    const DexClasses& squashed_classes =
        m_dexes_structure.get_current_dex_squashed_classes();
    classes.insert(classes.end(), squashed_classes.begin(),
                   squashed_classes.end());
    for (auto cls : plugin->additional_classes(m_outdex, classes)) {
      TRACE(IDEX, 4, "IDEX: Emitting %s-plugin-generated class :: %s",
            plugin->name().c_str(), SHOW(cls));
      m_dexes_structure.add_class_no_checks(cls);
      // If this is the primary dex, or if there are any betamap-ordered classes
      // in this dex, then we treat the additional classes as perf-sensitive, to
      // be conservative.
      if (dex_info.primary || dex_info.betamap_ordered) {
        cls->set_perf_sensitive(true);
      }
      additional_classes.insert(cls);
    }
  }

  {
    auto classes = m_dexes_structure.end_dex(dex_info);
    if (m_sort_remaining_classes) {
      std::vector<DexClass*> perf_sensitive_classes;
      using DexClassWithSortNum = std::pair<DexClass*, double>;
      std::vector<DexClassWithSortNum> classes_with_sort_num;
      std::vector<DexClass*> remaining_classes;
      using namespace method_profiles;
      dexmethods_profiled_comparator comparator(
          {},
          &m_conf.get_method_profiles(),
          &m_conf.get_method_sorting_allowlisted_substrings(),
          /* legacy_order */ false,
          /* min_appear_percent */ 1.0);
      for (auto cls : classes) {
        if (cls->is_perf_sensitive()) {
          perf_sensitive_classes.push_back(cls);
          continue;
        }
        double cls_sort_num = dexmethods_profiled_comparator::VERY_END;
        walk::methods(std::vector<DexClass*>{cls}, [&](DexMethod* method) {
          auto method_sort_num = comparator.get_overall_method_sort_num(method);
          if (method_sort_num < cls_sort_num) {
            cls_sort_num = method_sort_num;
          }
        });
        if (cls_sort_num < dexmethods_profiled_comparator::VERY_END) {
          classes_with_sort_num.emplace_back(cls, cls_sort_num);
          continue;
        }
        remaining_classes.push_back(cls);
      }
      always_assert(perf_sensitive_classes.size() +
                        classes_with_sort_num.size() +
                        remaining_classes.size() ==
                    classes.size());

      TRACE(IDEX, 2,
            "Skipping %zu perf sensitive, ordering %zu by method profiles, and "
            "sorting %zu classes",
            perf_sensitive_classes.size(), classes_with_sort_num.size(),
            remaining_classes.size());
      std::stable_sort(
          classes_with_sort_num.begin(), classes_with_sort_num.end(),
          [](const DexClassWithSortNum& a, const DexClassWithSortNum& b) {
            return a.second < b.second;
          });
      std::sort(remaining_classes.begin(), remaining_classes.end(),
                interdex::compare_dexclasses_for_compressed_size);
      // Rearrange classes so that...
      // - perf_sensitive_classes go first, then
      // - classes_with_sort_num that got ordered by the method profiles, and
      // finally
      // - remaining_classes
      classes.clear();
      classes.insert(classes.end(), perf_sensitive_classes.begin(),
                     perf_sensitive_classes.end());
      for (auto& p : classes_with_sort_num) {
        classes.push_back(p.first);
      }
      classes.insert(classes.end(), remaining_classes.begin(),
                     remaining_classes.end());
    }
    m_outdex.emplace_back(std::move(classes));
  }

  if (!m_emitting_scroll_set) {
    dex_info.scroll = false;
  }
  if (!m_emitting_bg_set) {
    dex_info.background = false;
  }
  if (!m_emitting_extended) {
    dex_info.extended = false;
  }

  // This is false by default and set to true everytime
  // a DEX contains classes already ordered by the betamap.
  // This resets the flag as this method advances to the next
  // writable DEX.
  dex_info.betamap_ordered = false;
}

} // namespace interdex
