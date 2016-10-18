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

using std::unordered_set;

/* Obfuscates a list of fields
 * RenamingContext - the context that we need to be able to do renaming for this
 *   field. Will not be modified and will be shared between all fields in a
 *   class.
 * ObfuscationState - keeps track of the new names we're trying to assign to
 *   fields, we update this to show what name we chose for a field. Also
 *   contains a set of all used names in this class because that needs to be
 *   updated every time we choose a name.
 */
void obfuscate_fields(const RenamingContext& context, ObfuscationState* state) {
  NameGenerator name_gen(context.ids_to_avoid, state->used_ids);
  for (const auto& field : context.fields) {
    if(!context.can_rename_field(field)) {
      TRACE(OBFUSCATE, 4, "Ignoring field %s because we shouldn't rename it\n",
          SHOW(field->get_name()));
      continue;
    }
    std::string new_name(name_gen.next_name());
    TRACE(OBFUSCATE, 2, "\tIntending to rename field (%s) %s:%s to %s\n",
        SHOW(field->get_type()), SHOW(field->get_class()),
        SHOW(field->get_name()), new_name.c_str());
    state->set_name_mapping(field, new_name);
  }
}

void obfuscate_methods(const std::list<DexMethod*>& methods,
                       ProguardMap* pg_map) {
  for (DexMethod* method : methods) {
    if (should_rename_method(method))
      rename_method(method, "");
  }
}

// Obfuscate methods and fields, updating the ProGuard
// map approriately to reflect renamings.
void obfuscate(Scope& classes, ProguardMap* pg_map) {
  ObfuscationState ob_state;
  unordered_set<std::string> ids_to_avoid;
  ClassVisitor publicVisitor(ClassVisitor::VisitFilter::NonPrivateOnly,
      FieldNameCollector(&ob_state.name_mapping, &ids_to_avoid));
  ClassVisitor allVisitor(ClassVisitor::VisitFilter::All,
      FieldNameCollector(&ob_state.name_mapping, &ids_to_avoid));
  ClassVisitor privateVisitor(ClassVisitor::VisitFilter::PrivateOnly,
      FieldNameCollector(&ob_state.name_mapping, &ids_to_avoid));

  for (DexClass* cls : classes) {
    always_assert_log(!cls->is_external(),
        "Shouldn't rename members of external classes.");
    TRACE(OBFUSCATE, 2, "Renaming the members of class %s\n",
        SHOW(cls->get_name()));

    // Reset class-specific state
    ids_to_avoid.clear();
    ob_state.used_ids.clear();

    walk_hierarchy(cls, &publicVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking hierarchies 1.1\n");
    walk_hierarchy(cls, &allVisitor, HierarchyDirection::VisitSubClasses);
    TRACE(OBFUSCATE, 3, "Finished walking hierarchies 1.2\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    obfuscate_fields(RenamingContext(cls->get_ifields(), ids_to_avoid, false),
        &ob_state);
    obfuscate_fields(RenamingContext(cls->get_sfields(), ids_to_avoid, false),
        &ob_state);
    TRACE(OBFUSCATE, 2, "Finished obfuscating publics\n");

    // Obfu private fields
    ids_to_avoid.clear();
    walk_hierarchy(cls, &publicVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking hierarchies 2.1\n");
    walk_hierarchy(cls, &allVisitor, HierarchyDirection::VisitNeither);
    TRACE(OBFUSCATE, 3, "Finished walking hierarchies 2.2\n");
    TRACE(OBFUSCATE, 3, "Avoiding");
    for (auto& id : ids_to_avoid) {
      TRACE(OBFUSCATE, 3, " %s\t", id.c_str());
    }
    for (auto& id : ob_state.used_ids) {
      TRACE(OBFUSCATE, 3, " %s\t", id.c_str());
    }
    TRACE(OBFUSCATE, 3, "\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    obfuscate_fields(RenamingContext(cls->get_ifields(), ids_to_avoid, true),
        &ob_state);
    obfuscate_fields(RenamingContext(cls->get_sfields(), ids_to_avoid, true),
        &ob_state);
    TRACE(OBFUSCATE, 2, "Finished obfuscating privates\n");
    // Dex entries need to be sorted and we may have disrupted that.

    /* Rename methods here in a similar manner, but be careful of more things
       - Interface implementors
       - library boundaries (taken care of)
       - overrides (same method name in same hierarchy)
    */

    // Make sure our renaming worked correctly (check each name in name_mapping
    // and name_cache for conflict)
    // Find all names that could conflict (anything public in hierarchy +
    // anything private in a subclass)

    //obfuscate_methods(cls->get_vmethods(), pg_map);
    //obfuscate_methods(cls->get_dmethods(), pg_map);
    ob_state.name_mapping.print();
  }

  // TODO: figure out if we need to verify if name conflicts exist for fields

  // Apply new names
  for (DexClass* cls : classes) {
    for (DexField* field : cls->get_ifields()) {
      if (ob_state.name_mapping.contains_field(field))
        rename_field(field, ob_state.name_mapping[field].get_name());
    }
    for (DexField* field : cls->get_sfields()) {
      if (ob_state.name_mapping.contains_field(field))
        rename_field(field, ob_state.name_mapping[field].get_name());
    }
    cls->get_ifields().sort(compare_dexfields);
    cls->get_sfields().sort(compare_dexfields);
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
}

void redex::ObfuscatePass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  obfuscate(scope, &cfg.get_proguard_map());
  post_dexen_changes(scope, stores);
}

static redex::ObfuscatePass s_pass;
