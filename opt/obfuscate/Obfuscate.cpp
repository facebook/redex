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
#include <list>

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
  for (DexField* field : context.fields) {
    if (!context.can_rename_field(field)) {
      TRACE(OBFUSCATE, 4, "Ignoring field %s because we shouldn't rename it\n",
          SHOW(field->get_name()));
      continue;
    }
    context.name_gen->find_new_name(&state->name_mapping[field]);
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
  SimpleNameGenerator simple_name_gen(ids_to_avoid, &ob_state.used_ids);
  StaticNameGenerator static_name_gen(ids_to_avoid, &ob_state.used_ids);
  FieldNameCollector name_collector(&(ob_state.name_mapping), &ids_to_avoid);
  ClassVisitor publicVisitor(ClassVisitor::VisitFilter::NonPrivateOnly,
      &name_collector);
  ClassVisitor allVisitor(ClassVisitor::VisitFilter::All,
      &name_collector);
  ClassVisitor privateVisitor(ClassVisitor::VisitFilter::PrivateOnly,
      &name_collector);

  TRACE(OBFUSCATE, 2, "Starting obfuscation of fields and methods\n");
  for (DexClass* cls : classes) {
    // First check if we will do anything on this class
    bool operate_on_ifields = contains_renamable_field(cls->get_ifields());
    bool operate_on_sfields = contains_renamable_field(cls->get_sfields());
    if (!operate_on_ifields && !operate_on_sfields) continue;
    always_assert_log(!cls->is_external(),
        "Shouldn't rename members of external classes.");
    TRACE(OBFUSCATE, 2, "Renaming the members of class %s\n",
        SHOW(cls->get_name()));

    // Reset class-specific state
    ids_to_avoid.clear();
    ob_state.used_ids.clear();
    static_name_gen.reset();
    simple_name_gen.reset();
    walk_hierarchy(cls, &publicVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking public supers\n");
    walk_hierarchy(cls, &allVisitor, HierarchyDirection::VisitSubClasses);
    TRACE(OBFUSCATE, 3, "Finished walking all subclasses\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    if (operate_on_ifields)
      obfuscate_fields(RenamingContext(cls->get_ifields(), ids_to_avoid,
          &simple_name_gen, false),
        &ob_state);
    if (operate_on_sfields)
      obfuscate_fields(RenamingContext(cls->get_sfields(), ids_to_avoid,
          &static_name_gen, false),
        &ob_state);
    TRACE(OBFUSCATE, 2, "Finished obfuscating publics\n");

    // Obfu private fields
    ids_to_avoid.clear();
    walk_hierarchy(cls, &publicVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking public supers\n");
    walk_hierarchy(cls, &allVisitor, HierarchyDirection::VisitNeither);

    // Keep this for all public ids in the class (they shouldn't conflict)
    if (operate_on_ifields)
      obfuscate_fields(RenamingContext(cls->get_ifields(), ids_to_avoid,
          &static_name_gen, true),
        &ob_state);
    if (operate_on_sfields)
      obfuscate_fields(RenamingContext(cls->get_sfields(), ids_to_avoid,
          &static_name_gen, true),
        &ob_state);

    // Make sure to bind the new names otherwise not all generators will assign
    // names to the members
    simple_name_gen.bind_names();
    static_name_gen.bind_names();
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
    TRACE(OBFUSCATE, 2, "Finished obfuscating privates\n");
    //obfuscate_methods(cls->get_vmethods(), pg_map);
    //obfuscate_methods(cls->get_dmethods(), pg_map);
  }
  ob_state.name_mapping.print_elements();

  TRACE(OBFUSCATE, 2, "Finished picking new names\n");

  // TODO: figure out if we need to verify if name conflicts exist for fields
  // ProGuard verifies the mapping at the end, but that seems to be only so
  // that they can support loading mappings and respecting ProGuard configs
  // for multi-stage obfuscation. We don't need to do that so I don't think we
  // need to check that there are no conflicts (assuming implementation is
  // correct)

  TRACE(OBFUSCATE, 2, "Transforming affected refs into defs\n");

  // Update any instructions with a field that is a ref to the corresponding
  // def for any field that we are going to rename. This allows us to in-place
  // rename the field def and have that change seen everywhere.
  walk_opcodes(classes,
    [](DexMethod*) { return true; },
    [&](DexMethod*, DexInstruction* instr) {
      // Only want field operations
      if (!is_ifield_op(instr->opcode()) &&
          !is_sfield_op(instr->opcode())) return;
      DexOpcodeField* field_instr = static_cast<DexOpcodeField*>(instr);

      DexField* field_ref = field_instr->field();
      if (field_ref->is_def()) return;
      TRACE(OBFUSCATE, 3, "Found a ref opcode\n");

      // Here we could use resolve_field to lookup the def, but this is
      // expensive, so we do resolution through ob_state which caches
      // and combines the lookup with the check for if we're changing the
      // field.
      DexField* field_def = ob_state.get_def_if_renamed(field_ref);
      if (field_def != nullptr) {
        TRACE(OBFUSCATE, 4, "Found a ref to fixup %s", SHOW(field_ref));
        field_instr->rewrite_field(field_def);
      }
    });

  TRACE(OBFUSCATE, 2, "Finished transforming refs\n");

  // Apply new names, recording what we're changing
  ob_state.commit_renamings_to_dex();
  // Sort the result because dexes have to be sorted
  for (DexClass* cls : classes) {
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
  TRACE(OBFUSCATE, 2, "Finished applying new names to defs\n");
}

void redex::ObfuscatePass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  obfuscate(scope, &cfg.get_proguard_map());
  post_dexen_changes(scope, stores);
}

static redex::ObfuscatePass s_pass;
