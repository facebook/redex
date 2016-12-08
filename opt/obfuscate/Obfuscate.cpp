/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Obfuscate.h"
#include "ObfuscateUtils.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ProguardMap.h"
#include "ReachableClasses.h"
#include "Trace.h"
#include "Transform.h"
#include "Walkers.h"
#include "Resolver.h"
#include "Devirtualizer.h"
#include <list>

namespace {

static const char* METRIC_REWRITTEN_FIELD_DEFS = "num_fields_renamed";
static const char* METRIC_REWRITTEN_METHOD_DEFS = "num_methods_renamed";
static const char* METRIC_BEFORE_COMMIT_SFIELD_RENAMES =
    "sfields_rename_attempted";
static const char* METRIC_BEFORE_COMMIT_IFIELD_RENAMES =
    "ifields_rename_attempted";
static const char* METRIC_BEFORE_COMMIT_DMETHOD_RENAMES =
    "dmethods_rename_attempted";
static const char* METRIC_BEFORE_COMMIT_VMETHOD_RENAMES =
    "vmethods_rename_attempted";


using std::unordered_set;

/* Obfuscates a list of members
 * RenamingContext - the context that we need to be able to do renaming for this
 *   member. Will not be modified and will be shared between all members in a
 *   class.
 * ObfuscationState - keeps track of the new names we're trying to assign to
 *   members, we update this to show what name we chose for a member. Also
 *   contains a set of all used names in this class because that needs to be
 *   updated every time we choose a name.
 * returns the number of find_new_name calls done
 */
template <class T, class R, class K>
int obfuscate_elems(const RenamingContext<T>& context,
    DexElemManager<T, R, K>& name_mapping) {
  int num_renames = 0;
  for (T elem : context.elems) {
    if (!context.can_rename_elem(elem) ||
        !name_mapping[elem]->should_rename()) {
      TRACE(OBFUSCATE, 4, "Ignoring member %s because we shouldn't rename it\n",
          SHOW(elem->get_name()));
      continue;
    }
    context.name_gen.find_new_name(name_mapping[elem]);
    num_renames++;
  }
  return num_renames;
}

void sort_members(std::vector<DexClass*>& classes) {
  // Sort the result because dexes have to be sorted
  for (DexClass* cls : classes) {
    cls->get_ifields().sort(compare_dexfields);
    cls->get_sfields().sort(compare_dexfields);
    cls->get_dmethods().sort(compare_dexmethods);
    cls->get_vmethods().sort(compare_dexmethods);
    // Debug logging
    TRACE(OBFUSCATE, 4, "Applying new names:\n  List of ifields\t");
    for (DexField* f : cls->get_ifields())
      TRACE(OBFUSCATE, 4, "%s\t", SHOW(f->get_name()));
    TRACE(OBFUSCATE, 4, "\n");
    TRACE(OBFUSCATE, 4, "  List of sfields\t");
    for (DexField* f : cls->get_sfields())
      TRACE(OBFUSCATE, 4, "%s\t", SHOW(f->get_name()));
    TRACE(OBFUSCATE, 4, "\n");
  }
  TRACE(OBFUSCATE, 2, "Finished applying new names to defs\n");
}

template<typename DexMember, typename DexMemberRef, typename K>
DexMember* find_renamable_ref(DexMember* ref,
    std::unordered_map<DexMember*, DexMember*>& ref_def_cache,
    DexElemManager<DexMember*, DexMemberRef, K>& name_mapping) {
  TRACE(OBFUSCATE, 3, "Found a ref opcode\n");
  DexMember* def = nullptr;
  auto member_itr = ref_def_cache.find(ref);
  if (member_itr != ref_def_cache.end()) {
    def = member_itr->second;
  } else {
    def = name_mapping.def_of_ref(ref);
  }
  ref_def_cache[ref] = def;
  return def;
}

void update_refs(Scope& scope, DexFieldManager& field_name_mapping,
    DexMethodManager& method_name_mapping) {
  std::unordered_map<DexField*, DexField*> f_ref_def_cache;
  std::unordered_map<DexMethod*, DexMethod*> m_ref_def_cache;
  walk_opcodes(scope,
    [](DexMethod*) { return true; },
    [&](DexMethod*, DexInstruction* instr) {
      if (instr->has_fields()) {
        DexOpcodeField* field_instr = static_cast<DexOpcodeField*>(instr);
        DexField* field_ref = field_instr->field();
        if (field_ref->is_def()) return;
        DexField* field_def =
            find_renamable_ref(field_ref, f_ref_def_cache, field_name_mapping);
        if (field_def != nullptr) {
          TRACE(OBFUSCATE, 4, "Found a ref to fixup %s", SHOW(field_ref));
          field_instr->rewrite_field(field_def);
        }
      } else if (instr->has_methods()) {
        DexOpcodeMethod* meth_instr = static_cast<DexOpcodeMethod*>(instr);
        DexMethod* method_ref = meth_instr->get_method();
        if (method_ref->is_def()) return;
        DexMethod* method_def =
            find_renamable_ref(method_ref, m_ref_def_cache,
                method_name_mapping);
        if (method_def != nullptr) {
          TRACE(OBFUSCATE, 4, "Found a ref to fixup %s", SHOW(method_ref));
          meth_instr->rewrite_method(method_def);
        }
      }
    });
}

// Prints out what proguard renames vs what we rename (only relevant if we're)
// running on a proguarded apk with a correct proguard mapping file.
/*void trace_proguard_delta(Scope& classes, ProguardMap* pg_map,
    DexMethodManager& method_name_manager, DexFieldManager& field_name_manager) {
  int pg_renamed_f = 0;
  int us_and_pg_f = 0;
  int us_not_pg_f = 0;
  int pg_not_us_f = 0;
  int vol_fields = 0;
  int pg_renamed_m = 0;
  int us_and_pg_m = 0;
  int us_not_pg_m = 0;
  int pg_not_us_m = 0;
  int pg_renamed_m1 = 0;
  int us_and_pg_m1 = 0;
  int us_not_pg_m1 = 0;
  int pg_not_us_m1 = 0;
  int renamable_not_renamed_f = 0;
  int renamable_not_renamed_m = 0;
  int renamable_not_renamed_m1 = 0;
  int renamable_not_renamed_m1_ourfl = 0;
  for (auto cls : classes) {
    if (cls->is_external()) continue;
    for (auto meth : cls->get_dmethods()) {
      auto deob_name = pg_map->deobfuscate_method(proguard_name(meth));
      size_t beg = deob_name.find(";") + 2;
      deob_name = deob_name.substr(beg, deob_name.find(":") - beg);
      bool pg_renamed_ = strcmp(deob_name.c_str(), SHOW(meth->get_name())) != 0;
      if (pg_renamed_) {
        pg_renamed_m++;
        if (method_name_manager[meth]->is_modified()) {
          us_and_pg_m++;
        } else {
          if (can_rename(meth))
            renamable_not_renamed_m++;
          pg_not_us_m++;
        }
      } else {
        if (method_name_manager[meth]->is_modified()) {
          us_not_pg_m++;
        }
      }
    }
    for (auto meth : cls->get_vmethods()) {
      auto deob_name = pg_map->deobfuscate_method(proguard_name(meth));
      size_t beg = deob_name.find(";") + 2;
      deob_name = deob_name.substr(beg, deob_name.find(":") - beg);
      bool pg_renamed_ = strcmp(deob_name.c_str(), SHOW(meth->get_name())) != 0;
      if (pg_renamed_) {
        pg_renamed_m1++;
        if (method_name_manager[meth]->is_modified()) {
          us_and_pg_m1++;
        } else {
          if (can_rename(meth)) {
            renamable_not_renamed_m1++;
            if (method_name_manager[meth]->should_rename())
              renamable_not_renamed_m1_ourfl++;
          }
          pg_not_us_m1++;
        }
      } else {
        if (method_name_manager[meth]->is_modified()) us_not_pg_m1++;
      }
    }
    for (auto field : cls->get_ifields()) {
      auto deob_name = pg_map->deobfuscate_field(proguard_name(field));
      size_t beg = deob_name.find(";") + 2;
      deob_name = deob_name.substr(beg, deob_name.find(":") - beg);
      bool pg_renamed_ = strcmp(deob_name.c_str(), SHOW(field->get_name())) != 0;
      if (pg_renamed_) {
        pg_renamed_f++;
        if (field_name_manager[field]->is_modified()) { us_and_pg_f++; }
        else {
          if (can_rename(field))
            renamable_not_renamed_f++;
          //if (is_volatile(field)) vol_fields++;
           pg_not_us_f++; }
      } else {
        if (field_name_manager[field]->is_modified()) us_not_pg_f++;
      }
    }
    for (auto field : cls->get_sfields()) {
      auto deob_name = pg_map->deobfuscate_field(proguard_name(field));
      size_t beg = deob_name.find(";") + 2;
      deob_name = deob_name.substr(beg, deob_name.find(":") - beg);
      bool pg_renamed_ = strcmp(deob_name.c_str(), SHOW(field->get_name())) != 0;
      if (pg_renamed_) {
        pg_renamed_f++;
        if (field_name_manager[field]->is_modified()) { us_and_pg_f++; }
        else {
          if (can_rename(field))
            renamable_not_renamed_f++;
          //if (is_volatile(field)) vol_fields++;
           pg_not_us_f++; }
      } else {
        if (field_name_manager[field]->is_modified()) us_not_pg_f++;
      }
    }
  }
  TRACE(OBFUSCATE, 2, "Proguard renamed %d, we renamed %d of those\n We renamed"
      " %d that PG did not, PG renamed %d that we did not\n %s volatile fields ignored by us\n",
      pg_renamed_f, us_and_pg_f, us_not_pg_f, pg_not_us_f, vol_fields);
  TRACE(OBFUSCATE, 2, "DMETHODS: Proguard renamed %d, we renamed %d of those\n"
      " We renamed %d that PG did not, PG renamed %d that we did not\n",
      pg_renamed_m, us_and_pg_m, us_not_pg_m, pg_not_us_m);
  TRACE(OBFUSCATE, 2, "VMETHODS: Proguard renamed %d, we renamed %d of those\n"
      " We renamed %d that PG did not, PG renamed %d that we did not, our flags %d\n",
      pg_renamed_m1, us_and_pg_m1, us_not_pg_m1, pg_not_us_m1,
      renamable_not_renamed_m1_ourfl);
  TRACE(OBFUSCATE, 2, "Renamable not renamed:\nFields: %d\nDmethods: %d\n Vmethods %d\n",
      renamable_not_renamed_f, renamable_not_renamed_m, renamable_not_renamed_m1);
}*/

// Obfuscate methods and fields, updating the ProGuard
// map approriately to reflect renamings.
void obfuscate(Scope& classes, ProguardMap* pg_map, PassManager& pass_mgr) {
  MethodLinkInfo link_manager = link_methods(classes);
  DexFieldManager field_name_manager(new_dex_field_manager());
  DexMethodManager& method_name_manager(link_manager.name_manager);
  for (auto cls_intfs : link_manager.class_interfaces) {
    TRACE(OBFUSCATE, 1, "Class: %s\n", SHOW(cls_intfs.first));
    for (auto intf : cls_intfs.second) {
      TRACE(OBFUSCATE, 1, "\tIntf: %s\n", SHOW(intf));
    }
  }

  TRACE(OBFUSCATE, 2, "Starting obfuscation of fields and methods\n");
  for (DexClass* cls : classes) {
    always_assert_log(!cls->is_external(),
        "Shouldn't rename members of external classes.");
    // First check if we will do anything on this class
    bool operate_on_ifields = contains_renamable_elem(cls->get_ifields(), field_name_manager);
    bool operate_on_sfields = contains_renamable_elem(cls->get_sfields(), field_name_manager);
    bool operate_on_dmethods = contains_renamable_elem(cls->get_dmethods(), method_name_manager);
    bool operate_on_vmethods = contains_renamable_elem(cls->get_vmethods(), method_name_manager);
    if (operate_on_ifields || operate_on_sfields) {
      FieldObfuscationState f_ob_state;
      SimpleNameGenerator<DexField*> simple_name_generator(
          f_ob_state.ids_to_avoid, f_ob_state.used_ids);
      StaticFieldNameGenerator static_name_generator(
          f_ob_state.ids_to_avoid, f_ob_state.used_ids);

      TRACE(OBFUSCATE, 2, "Renaming the fields of class %s\n",
          SHOW(cls->get_name()));

      f_ob_state.populate_ids_to_avoid(cls, field_name_manager, true);

      // Keep this for all public ids in the class (they shouldn't conflict)
      if (operate_on_ifields) {
        int renamed = obfuscate_elems(FieldRenamingContext(cls->get_ifields(),
            f_ob_state.ids_to_avoid,
            simple_name_generator, false),
          field_name_manager);
        pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_IFIELD_RENAMES, renamed);
      }
      if (operate_on_sfields) {
        int renamed = obfuscate_elems(FieldRenamingContext(cls->get_sfields(),
            f_ob_state.ids_to_avoid,
            static_name_generator, false),
          field_name_manager);
        pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_SFIELD_RENAMES, renamed);
      }

      // Obfu private fields
      f_ob_state.ids_to_avoid.clear();
      f_ob_state.populate_ids_to_avoid(cls, field_name_manager, false);

      // Keep this for all public ids in the class (they shouldn't conflict)
      if (operate_on_ifields) {
        int renamed = obfuscate_elems(FieldRenamingContext(cls->get_ifields(),
            f_ob_state.ids_to_avoid,
            simple_name_generator, true),
          field_name_manager);
        pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_IFIELD_RENAMES, renamed);
      }
      if (operate_on_sfields) {
        int renamed = obfuscate_elems(FieldRenamingContext(cls->get_sfields(),
            f_ob_state.ids_to_avoid,
            static_name_generator, true),
          field_name_manager);
        pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_SFIELD_RENAMES, renamed);
      }

      // Make sure to bind the new names otherwise not all generators will assign
      // names to the members
      static_name_generator.bind_names();
    }

    // =========== Obfuscate Methods Below ==========
    if (operate_on_vmethods || operate_on_dmethods) {
      MethodObfuscationState m_ob_state;
      MethodNameGenerator simple_name_gen(m_ob_state.ids_to_avoid,
          m_ob_state.used_ids);

      TRACE(OBFUSCATE, 2, "Renaming the methods of class %s\n",
                SHOW(cls->get_name()));
      m_ob_state.populate_ids_to_avoid(cls, method_name_manager, true);

      // Keep this for all public ids in the class (they shouldn't conflict)
      if (operate_on_dmethods) {
        int renamed = obfuscate_elems(MethodRenamingContext(cls->get_dmethods(),
            m_ob_state.ids_to_avoid, simple_name_gen, method_name_manager, false),
          method_name_manager);
        pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_DMETHOD_RENAMES, renamed);
      }
      if (operate_on_vmethods) {
        // Gather used names from all classes that contain a linked member
        // (walk hierarchies of all implementations of any interfaces
        // implemented by members in this class)
        // Make sure that if we're an interface, we correctly get the conflict set

        if (link_manager.intf_conflict_set.count(cls->get_type()) == 0) {
          // if we're at an interface, vmethod renaming won't work correctly,
          // just wait until we're renamed from one of the classes that
          // implements this interface
          for (auto intf : link_manager.class_interfaces[cls->get_type()])
            for (auto meth : link_manager.intf_conflict_set[intf])
              m_ob_state.ids_to_avoid.insert(
                  method_name_manager[meth]->get_name());

          int renamed = obfuscate_elems(MethodRenamingContext(cls->get_vmethods(),
              m_ob_state.ids_to_avoid, simple_name_gen, method_name_manager, false),
            method_name_manager);
          pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_VMETHOD_RENAMES, renamed);
        }
      }

      // Obfu private methods
      m_ob_state.ids_to_avoid.clear();
      m_ob_state.populate_ids_to_avoid(cls, method_name_manager, false);

      if (operate_on_dmethods) {
        int renamed = obfuscate_elems(MethodRenamingContext(cls->get_dmethods(),
            m_ob_state.ids_to_avoid, simple_name_gen, method_name_manager, true),
          method_name_manager);
        pass_mgr.incr_metric(METRIC_BEFORE_COMMIT_DMETHOD_RENAMES, renamed);
      }
    }
  }
  field_name_manager.print_elements();
  method_name_manager.print_elements();


  TRACE(OBFUSCATE, 2, "Finished picking new names\n");

  // Update any instructions with a member that is a ref to the corresponding
  // def for any field that we are going to rename. This allows us to in-place
  // rename the field def and have that change seen everywhere.
  update_refs(classes, field_name_manager, method_name_manager);

  TRACE(OBFUSCATE, 2, "Finished transforming refs\n");

  // Apply new names, recording what we're changing
  TRACE(OBFUSCATE, 1, "Fields:");
  int rewritten_field_defs = field_name_manager.commit_renamings_to_dex();
  pass_mgr.incr_metric(METRIC_REWRITTEN_FIELD_DEFS, rewritten_field_defs);
  TRACE(OBFUSCATE, 1, "Methods:");
  int rewritten_method_defs = method_name_manager.commit_renamings_to_dex();
  pass_mgr.incr_metric(METRIC_REWRITTEN_METHOD_DEFS, rewritten_method_defs);
  sort_members(classes);
}

} // end namespace

void ObfuscatePass::run_pass(DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(OBFUSCATE, 1, "ObfuscatePass not run because no ProGuard configuration was provided.");
    return;
  }
  clock_t start = std::clock();
  auto scope = build_class_scope(stores);
  obfuscate(scope, &cfg.get_proguard_map(), mgr);
  clock_t end = std::clock();
  TRACE(OBFUSCATE, 1, "Time taken %.3fs\n", (1.0 * (end - start)) / CLOCKS_PER_SEC);
}

static ObfuscatePass s_pass;
