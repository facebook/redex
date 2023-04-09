/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include "IOUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPassPlugin.h"
#include "MethodProfiles.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "StlUtil.h"
#include "StringUtil.h"
#include "Walkers.h"
#include "WorkQueue.h"
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
    const DexClass* cls,
    interdex::MethodRefs* mrefs,
    interdex::FieldRefs* frefs,
    interdex::TypeRefs* trefs,
    interdex::TypeRefs* itrefs) {
  std::vector<DexMethodRef*> method_refs;
  std::vector<DexFieldRef*> field_refs;
  std::vector<DexType*> type_refs;
  std::vector<DexType*> init_type_refs;
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(type_refs);
  cls->gather_init_classes(init_type_refs);

  for (const auto& plugin : plugins) {
    plugin->gather_refs(cls, method_refs, field_refs, type_refs,
                        init_type_refs);
  }

  mrefs->insert(method_refs.begin(), method_refs.end());
  frefs->insert(field_refs.begin(), field_refs.end());
  trefs->insert(type_refs.begin(), type_refs.end());
  itrefs->insert(init_type_refs.begin(), init_type_refs.end());
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

bool InterDex::should_skip_class_due_to_plugin(DexClass* clazz) const {
  for (const auto& plugin : m_plugins) {
    if (plugin->should_skip_class(clazz)) {
      TRACE(IDEX, 4, "IDEX: Skipping class from %s :: %s",
            plugin->name().c_str(), SHOW(clazz));
      return true;
    }
  }

  return false;
}

InterDex::EmitResult InterDex::emit_class(
    EmittingState& emitting_state,
    DexInfo& dex_info,
    DexClass* clazz,
    bool check_if_skip,
    bool perf_sensitive,
    DexClass** canary_cls,
    std::optional<FlushOutDexResult>* opt_fodr) const {
  if (is_canary(clazz)) {
    // Nothing to do here.
    return {false, false};
  }

  if (emitting_state.dexes_structure.has_class(clazz)) {
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
  TypeRefs clazz_itrefs;
  gather_refs(m_plugins, clazz, &clazz_mrefs, &clazz_frefs, &clazz_trefs,
              &clazz_itrefs);

  bool fits_current_dex =
      emitting_state.dexes_structure.add_class_to_current_dex(
          clazz_mrefs, clazz_frefs, clazz_trefs, clazz_itrefs, clazz);
  if (!fits_current_dex) {
    auto fodr = flush_out_dex(emitting_state, dex_info, *canary_cls);
    *canary_cls = get_canary_cls(emitting_state, dex_info);

    if (opt_fodr) {
      *opt_fodr = fodr;
      return {false, true};
    }

    post_process_dex(emitting_state, fodr);

    emitting_state.dexes_structure.add_class_no_checks(
        clazz_mrefs, clazz_frefs, clazz_trefs, clazz_itrefs, clazz);
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

      emit_class(m_emitting_state, primary_dex_info, cls,
                 /* check_if_skip */ true,
                 /* perf_sensitive */ true, /* canary_cls */ nullptr);
      coldstart_classes_in_primary++;
    }
  }

  // Now add the rest.
  for (DexClass* cls : primary_dex) {
    emit_class(m_emitting_state, primary_dex_info, cls,
               /* check_if_skip */ true,
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

  auto fodr = flush_out_dex(m_emitting_state, primary_dex_info,
                            /* canary_cls */ nullptr);
  post_process_dex(m_emitting_state, fodr);

  // Double check only 1 dex was created.
  always_assert_log(
      m_emitting_state.dexes_structure.get_num_dexes() == 1,
      "[error]: Primary dex doesn't fit in only 1 dex anymore :|, but in %zu\n",
      m_emitting_state.dexes_structure.get_num_dexes());
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
            auto fodr = flush_out_dex(m_emitting_state, dex_info, *canary_cls);
            post_process_dex(m_emitting_state, fodr);
            *canary_cls = get_canary_cls(m_emitting_state, dex_info);
            if (end_marker == cold_start_end_marker) {
              dex_info.coldstart = false;
            }
          } else {
            reset_coldstart_on_overflow = true;
            TRACE(IDEX, 2, "Not flushing out marker %s to fill dex.",
                  SHOW(type));
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
      auto res =
          emit_class(m_emitting_state, dex_info, cls, /* check_if_skip */ true,
                     /* perf_sensitive */ true, canary_cls);

      if (res.overflowed && reset_coldstart_on_overflow) {
        dex_info.coldstart = false;
        reset_coldstart_on_overflow = false;
        TRACE(IDEX, 2, "Flushing cold-start after non-flushed end marker.");
      }
    }
  }

  // Now emit the classes we omitted from the original coldstart set.
  TRACE(IDEX, 2, "Emitting %zu interdex types (reset_coldstart_on_overflow=%d)",
        interdex_types.size(), reset_coldstart_on_overflow);
  for (DexType* type : interdex_types) {
    DexClass* cls = type_class(type);

    if (cls && unreferenced_classes.count(cls)) {
      auto res =
          emit_class(m_emitting_state, dex_info, cls, /* check_if_skip */ true,
                     /* perf_sensitive */ false, canary_cls);

      if (res.overflowed && reset_coldstart_on_overflow) {
        dex_info.coldstart = false;
        reset_coldstart_on_overflow = false;
        TRACE(IDEX, 2, "Flushing cold-start after non-flushed end marker.");
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

  if (reset_coldstart_on_overflow) {
    TRACE(IDEX, 2, "No overflow after cold-start dex, flushing now.");
    auto fodr = flush_out_dex(m_emitting_state, dex_info, *canary_cls);
    post_process_dex(m_emitting_state, fodr);
    *canary_cls = get_canary_cls(m_emitting_state, dex_info);
    dex_info.coldstart = false;
  }
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
                  all_set.count(cur) != 0 ||
                  cur_cls->rstate.has_interdex_subgroup()) {
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

void InterDex::init_cross_dex_ref_minimizer() {
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

  std::vector<DexClass*> classes_to_insert;
  // Emit classes using some algorithm to group together classes which
  // tend to share the same refs.
  for (DexClass* cls : m_scope) {
    // Don't bother with classes that emit_class will skip anyway.
    if (is_canary(cls) || m_emitting_state.dexes_structure.has_class(cls)) {
      continue;
    }

    // Don't bother with classes that emit_class will skip anyway
    if (should_skip_class_due_to_plugin(cls)) {
      // Skipping a class due to a plugin might mean that (members of) of the
      // class will get emitted later via the additional-class mechanism.
      // So we'll also sample those classes here.
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
  for (auto cls :
       m_emitting_state.dexes_structure.get_current_dex().get_classes()) {
    m_cross_dex_ref_minimizer.sample(cls);
    m_cross_dex_ref_minimizer.insert(cls);
    m_cross_dex_ref_minimizer.erase(cls, /* emitted */ true);
  }
}

void InterDex::emit_remaining_classes(DexInfo& dex_info,
                                      DexClass** canary_cls) {
   m_current_classes_when_emitting_remaining =
      m_emitting_state.dexes_structure.get_current_dex().size();

  if (!m_minimize_cross_dex_refs) {
    for (DexClass* cls : m_scope) {
      emit_class(m_emitting_state, dex_info, cls, /* check_if_skip */ true,
                 /* perf_sensitive */ false, canary_cls);
    }
    return;
  }

  init_cross_dex_ref_minimizer();

  if (m_minimize_cross_dex_refs_explore_alternatives <= 1) {
    emit_remaining_classes_legacy(dex_info, canary_cls);
  } else {
    emit_remaining_classes_exploring_alternatives(dex_info, canary_cls);
  }
}

void InterDex::emit_remaining_classes_legacy(DexInfo& dex_info,
                                             DexClass** canary_cls) {
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

    std::optional<FlushOutDexResult> opt_fodr;
    bool emitted =
        emit_class(m_emitting_state, dex_info, cls, /* check_if_skip */ false,
                   /* perf_sensitive */ false, canary_cls, &opt_fodr);
    if (opt_fodr) {
      always_assert(!emitted);
      post_process_dex(m_emitting_state, *opt_fodr);
      m_cross_dex_ref_minimizer.reset();
      pick_worst = true;
      continue;
    }

    m_cross_dex_ref_minimizer.erase(cls, emitted);

    pick_worst = pick_worst && !emitted;
  }
}

void InterDex::emit_remaining_classes_exploring_alternatives(
    DexInfo& dex_info, DexClass** canary_cls) {
  // Strategy for picking the next class to emit in a dex:
  // - at the beginning of a new dex, consider a "seed" class, i.e. a class
  //   with a high seed weight
  // - otherwise, pick the "best" class according to the priority scheme that
  //   prefers classes that share many applied refs and bring in few unapplied
  //   refs
  // For each dex, we explore a set of alternatives in parallel, choosing the
  // "best" one to continue with. We use a cost function that tries to minimize
  // the remaining "difficulty" of assigning the remaining classes to dexes.

  struct Alternative {
    EmittingState emitting_state;
    DexInfo dex_info;
    cross_dex_ref_minimizer::CrossDexRefMinimizer cross_dex_ref_minimizer;
    DexClass* canary_cls;
    std::vector<FlushOutDexResult> fodrs;

    double fill_current_dex(const InterDex* inter_dex, DexClass* seed_cls) {
      while (!cross_dex_ref_minimizer.empty()) {
        DexClass* cls;
        if (seed_cls) {
          cls = seed_cls;
          seed_cls = nullptr;
        } else {
          // Default case
          cls = cross_dex_ref_minimizer.front();
        }

        std::optional<FlushOutDexResult> opt_fodr;
        bool emitted = inter_dex->emit_class(emitting_state, dex_info, cls,
                                             /* check_if_skip */ false,
                                             /* perf_sensitive */ false,
                                             &canary_cls, &opt_fodr);
        if (opt_fodr) {
          always_assert(!emitted);
          cross_dex_ref_minimizer.reset();
          fodrs.push_back(*opt_fodr);
          break;
        }

        cross_dex_ref_minimizer.erase(cls, emitted);
      }
      return cross_dex_ref_minimizer.get_remaining_difficulty();
    }
  };

  TRACE(IDEX, 1,
        "Finding cross-dex-ref-minimization solutions, considering %" PRId64
        " alternatives at each step",
        m_minimize_cross_dex_refs_explore_alternatives);

  auto best = std::make_unique<Alternative>(
      (Alternative){std::move(m_emitting_state), dex_info,
                    std::move(m_cross_dex_ref_minimizer), *canary_cls,
                    /* fodrs */ {}});

  bool first = true;
  while (!best->cross_dex_ref_minimizer.empty()) {
    auto worst_classes = best->cross_dex_ref_minimizer.worst(
        m_minimize_cross_dex_refs_explore_alternatives);
    if (first) {
      first = false;
      worst_classes.push_back(nullptr);
    }

    std::unique_ptr<Alternative> last;
    std::swap(last, best);
    std::mutex best_mutex;
    double best_remaining_difficulty = 0;
    size_t best_index = 0;

    workqueue_run_for<size_t>(
        0, worst_classes.size(),
        [this, &worst_classes, &last, &best, &best_remaining_difficulty,
         &best_index, &best_mutex](size_t index) {
          auto seed_cls = worst_classes.at(index);

          auto alt = std::make_unique<Alternative>(*last);
          auto remaining_difficulty = alt->fill_current_dex(this, seed_cls);

          TRACE(IDEX, 2,
                "Found cross-dex-ref-minimization solution with %f remaining "
                "difficulity at index %zu",
                remaining_difficulty, index);

          std::lock_guard<std::mutex> lock_guard(best_mutex);
          if (!best || remaining_difficulty < best_remaining_difficulty ||
              (remaining_difficulty == best_remaining_difficulty &&
               index < best_index)) {
            best = std::move(alt);
            best_remaining_difficulty = remaining_difficulty;
            best_index = index;
          }
        });
  }

  m_emitting_state = std::move(best->emitting_state);
  dex_info = best->dex_info;
  m_cross_dex_ref_minimizer = std::move(best->cross_dex_ref_minimizer);
  *canary_cls = best->canary_cls;
  for (const auto& fodr : best->fodrs) {
    post_process_dex(m_emitting_state, fodr);
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
    TypeRefs clazz_itrefs;
    gather_refs(m_plugins, cls, &clazz_mrefs, &clazz_frefs, &clazz_trefs,
                &clazz_itrefs);

    m_emitting_state.dexes_structure.add_class_no_checks(
        clazz_mrefs, clazz_frefs, clazz_trefs, clazz_itrefs, cls);
  }

  // Emit all no matter what it is.
  if (!m_emitting_state.dexes_structure.get_current_dex().empty()) {
    auto fodr =
        flush_out_dex(m_emitting_state, dex_info, /* canary_cls */ nullptr);
    post_process_dex(m_emitting_state, fodr);
  }

  TRACE(IDEX, 7, "IDEX: force_single_dex dex number: %zu",
        m_emitting_state.dexes_structure.get_num_dexes());
  print_stats(&m_emitting_state.dexes_structure);
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
  DexClass* canary_cls = get_canary_cls(m_emitting_state, dex_info);
  emit_interdex_classes(dex_info, m_interdex_types, unreferenced_classes,
                        &canary_cls);

  auto json_classes = m_cross_dex_ref_minimizer.get_json_classes();
  Json::Value json_first_dex;
  if (json_classes) {
    json_first_dex = m_cross_dex_ref_minimizer.get_json_class_indices(
        m_emitting_state.dexes_structure.get_current_dex().get_classes());
  }

  // Now emit the classes that weren't specified in the head or primary list.
  auto remaining_classes_first_dex_idx = m_emitting_state.outdex.size();
  emit_remaining_classes(dex_info, &canary_cls);

  // Emit what is left, if any.
  if (!m_emitting_state.dexes_structure.get_current_dex().empty()) {
    auto fodr = flush_out_dex(m_emitting_state, dex_info, canary_cls);
    post_process_dex(m_emitting_state, fodr);
    canary_cls = nullptr;
  }

  if (json_classes) {
    Json::Value json_solution = Json::arrayValue;
    for (size_t dex_idx = remaining_classes_first_dex_idx;
         dex_idx < m_emitting_state.outdex.size();
         dex_idx++) {
      auto& dex = m_emitting_state.outdex.at(dex_idx);
      if (!dex.empty()) {
        json_solution.append(
            m_cross_dex_ref_minimizer.get_json_class_indices(dex));
      }
    }
    Json::Value json_limits;
    json_limits["types"] =
        int(m_emitting_state.dexes_structure.get_trefs_limit());
    json_limits["fields"] =
        int(m_emitting_state.dexes_structure.get_frefs_limit());
    json_limits["methods"] =
        int(m_emitting_state.dexes_structure.get_mrefs_limit());
    Json::Value json_file;
    json_file["limits"] = json_limits;
    json_file["first_dex"] = json_first_dex;
    json_file["solution"] = json_solution;
    json_file["mapping"] = m_cross_dex_ref_minimizer.get_json_mapping();
    json_file["classes"] = *json_classes;
    write_string_to_file(
        m_conf.metafile("interdex-cross-ref-minimization.json"),
        json_file.toStyledString());
  }

  // Emit dex info manifest
  if (m_asset_manager.has_secondary_dex_dir()) {
    auto mixed_mode_file = m_asset_manager.new_asset_file("dex_manifest.txt");
    auto mixed_mode_fh = FileHandle(*mixed_mode_file);
    mixed_mode_fh.seek_end();
    std::stringstream ss;
    int ordinal = 0;
    for (const auto& info : m_emitting_state.dex_infos) {
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
                        m_emitting_state.dexes_structure.get_num_dexes() <
                            MAX_DEX_NUM,
                    "Bailing, max dex number surpassed %zu\n",
                    m_emitting_state.dexes_structure.get_num_dexes());

  print_stats(&m_emitting_state.dexes_structure);
}

void InterDex::run_on_nonroot_store() {
  TRACE(IDEX, 2, "IDEX: Running on non-root store");
  auto canary_cls = get_canary_cls(m_emitting_state, EMPTY_DEX_INFO);
  for (DexClass* cls : m_scope) {
    emit_class(m_emitting_state, EMPTY_DEX_INFO, cls,
               /* check_if_skip */ false,
               /* perf_sensitive */ false, &canary_cls);
  }

  // Emit what is left, if any.
  if (!m_emitting_state.dexes_structure.get_current_dex().empty()) {
    auto fodr = flush_out_dex(m_emitting_state, EMPTY_DEX_INFO, canary_cls);
    post_process_dex(m_emitting_state, fodr);
  }

  print_stats(&m_emitting_state.dexes_structure);
}

void InterDex::add_dexes_from_store(const DexStore& store) {
  auto canary_cls = get_canary_cls(m_emitting_state, EMPTY_DEX_INFO);
  const auto& dexes = store.get_dexen();
  for (const DexClasses& classes : dexes) {
    for (DexClass* cls : classes) {
      emit_class(m_emitting_state, EMPTY_DEX_INFO, cls,
                 /* check_if_skip */ false,
                 /* perf_sensitive */ false, &canary_cls);
    }
  }
  auto fodr = flush_out_dex(m_emitting_state, EMPTY_DEX_INFO, canary_cls);
  post_process_dex(m_emitting_state, fodr);
}

void InterDex::set_clinit_methods_if_needed(DexClass* cls) const {
  using namespace dex_asm;

  if (m_methods_for_canary_clinit_reference.empty()) {
    // No methods to call from clinit; don't create clinit.
    return;
  }

  if (cls->get_clinit()) {
    // We already created and added a clinit.
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
DexClass* InterDex::get_canary_cls(EmittingState& emitting_state,
                                   DexInfo& dex_info) const {
  if (!m_emit_canaries || dex_info.primary) {
    return nullptr;
  }
  int dexnum = emitting_state.dexes_structure.get_num_dexes();
  DexClass* canary_cls;
  {
    static std::mutex canary_mutex;
    std::lock_guard<std::mutex> lock_guard(canary_mutex);
    canary_cls = create_canary(dexnum);
    set_clinit_methods_if_needed(canary_cls);
  }
  MethodRefs clazz_mrefs;
  FieldRefs clazz_frefs;
  TypeRefs clazz_trefs;
  std::vector<DexType*> clazz_itrefs;
  canary_cls->gather_methods(clazz_mrefs);
  canary_cls->gather_fields(clazz_frefs);
  canary_cls->gather_types(clazz_trefs);
  canary_cls->gather_init_classes(clazz_itrefs);
  emitting_state.dexes_structure.add_refs_no_checks(
      clazz_mrefs, clazz_frefs, clazz_trefs,
      TypeRefs(clazz_itrefs.begin(), clazz_itrefs.end()));
  return canary_cls;
}

/**
 * This needs to be called before getting to the next dex.
 */
InterDex::FlushOutDexResult InterDex::flush_out_dex(
    EmittingState& emitting_state,
    DexInfo& dex_info,
    DexClass* canary_cls) const {

  int dexnum = emitting_state.dexes_structure.get_num_dexes();
  if (dex_info.primary) {
    TRACE(IDEX, 2, "Writing out primary dex with %zu classes.",
          emitting_state.dexes_structure.get_current_dex().size());
  } else {
    TRACE(IDEX, 2,
          "Writing out secondary dex number %zu, which is %s of coldstart, "
          "%s of extended set, %s of background set, %s scroll "
          "classes and has %zu classes.",
          emitting_state.dexes_structure.get_num_secondary_dexes() + 1,
          (dex_info.coldstart ? "part of" : "not part of"),
          (dex_info.extended ? "part of" : "not part of"),
          (dex_info.background ? "part of" : "not part of"),
          (dex_info.scroll ? "has" : "doesn't have"),
          emitting_state.dexes_structure.get_current_dex().size());
  }

  // Add the Canary class, if any.
  if (canary_cls) {
    always_assert(emitting_state.dexes_structure.current_dex_has_tref(
        canary_cls->get_type()));

    // Properly try to insert the class.

    MethodRefs clazz_mrefs;
    FieldRefs clazz_frefs;
    TypeRefs clazz_trefs;
    TypeRefs clazz_itrefs;
    gather_refs(m_plugins, canary_cls, &clazz_mrefs, &clazz_frefs, &clazz_trefs,
                &clazz_itrefs);

    bool canary_added = emitting_state.dexes_structure.add_class_to_current_dex(
        clazz_mrefs, clazz_frefs, clazz_trefs, clazz_itrefs, canary_cls);
    always_assert(canary_added);

    emitting_state.dex_infos.emplace_back(
        std::make_tuple(canary_cls->get_name()->str_copy(), dex_info));
  }

  FlushOutDexResult fodr{emitting_state.outdex.size(),
                         dex_info.primary || dex_info.betamap_ordered};
  auto classes = emitting_state.dexes_structure.end_dex(dex_info);
  emitting_state.outdex.emplace_back(std::move(classes));

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

  return fodr;
}

void InterDex::post_process_dex(EmittingState& emitting_state,
                                const FlushOutDexResult& fodr) const {
  auto& classes = emitting_state.outdex.at(fodr.dex_count);

  for (auto& plugin : m_plugins) {
    for (auto cls : plugin->additional_classes(fodr.dex_count, classes)) {
      TRACE(IDEX, 4, "IDEX: Emitting %s-plugin-generated class :: %s",
            plugin->name().c_str(), SHOW(cls));
      classes.push_back(cls);
      // If this is the primary dex, or if there are any betamap-ordered
      // classes in this dex, then we treat the additional classes as
      // perf-sensitive, to be conservative.
      if (fodr.primary_or_betamap_ordered) {
        cls->set_perf_sensitive(true);
      }
    }
  }
}

} // namespace interdex
