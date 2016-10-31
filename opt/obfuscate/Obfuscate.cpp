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

/* Obfuscates a list of members
 * RenamingContext - the context that we need to be able to do renaming for this
 *   member. Will not be modified and will be shared between all members in a
 *   class.
 * ObfuscationState - keeps track of the new names we're trying to assign to
 *   members, we update this to show what name we chose for a member. Also
 *   contains a set of all used names in this class because that needs to be
 *   updated every time we choose a name.
 */
template <class T, class K>
void obfuscate_elems(const RenamingContext<T>& context, ObfuscationState<T, K>* state) {
  for (T elem : context.elems) {
    if (!context.can_rename_elem(elem)) {
      TRACE(OBFUSCATE, 4, "Ignoring member %s because we shouldn't rename it\n",
          SHOW(elem->get_name()));
      continue;
    }
    context.name_gen->find_new_name((*state->name_mapping)[elem]);
  }
}

// Obfuscate methods and fields, updating the ProGuard
// map approriately to reflect renamings.
void obfuscate(Scope& classes, ProguardMap* pg_map) {
  // Create Field Obfuscation State
  FieldObfuscationState f_ob_state(new DexFieldManager());
  // Field things
  unordered_set<std::string> used_field_names;
  SimpleNameGenerator<DexField*> simple_field_name_gen(
      used_field_names, &f_ob_state.used_ids);
  StaticFieldNameGenerator static_field_name_gen(
      used_field_names, &f_ob_state.used_ids);
  FieldNameCollector field_name_collector(
      f_ob_state.name_mapping.get(), &used_field_names);
  ClassVisitor publicFieldVisitor(ClassVisitor::VisitFilter::NonPrivateOnly,
      &field_name_collector);
  ClassVisitor allFieldVisitor(ClassVisitor::VisitFilter::All,
      &field_name_collector);
  ClassVisitor privateFieldVisitor(ClassVisitor::VisitFilter::PrivateOnly,
      &field_name_collector);

  // Method things
  MethodObfuscationState m_ob_state(new DexMethodManager());
  // TODO: currently assume we don't want to cause a bunch of overloading ??
  unordered_set<std::string> used_method_names;
  SimpleNameGenerator<DexMethod*> simple_method_name_gen(
      used_method_names, &m_ob_state.used_ids);
  MethodNameCollector method_name_collector(
      m_ob_state.name_mapping.get(), &used_method_names);
  ClassVisitor publicMethodVisitor(ClassVisitor::VisitFilter::NonPrivateOnly,
      &method_name_collector);
  ClassVisitor allMethodVisitor(ClassVisitor::VisitFilter::All,
      &method_name_collector);
  ClassVisitor privateMethodVisitor(ClassVisitor::VisitFilter::PrivateOnly,
      &method_name_collector);

  TRACE(OBFUSCATE, 2, "Starting obfuscation of fields and methods\n");
  for (DexClass* cls : classes) {
    always_assert_log(!cls->is_external(),
        "Shouldn't rename members of external classes.");
    // First check if we will do anything on this class
    bool operate_on_ifields = contains_renamable_elem(cls->get_ifields());
    bool operate_on_sfields = contains_renamable_elem(cls->get_sfields());
    bool operate_on_dmethods = contains_renamable_elem(cls->get_dmethods());
    bool operate_on_vmethods = false;//contains_renamable_elem(cls->get_vmethods());
    // TODO: is there a simpler check for this?
    if (!operate_on_ifields && !operate_on_sfields &&
        !operate_on_dmethods && !operate_on_vmethods) continue;
    TRACE(OBFUSCATE, 2, "Renaming the members of class %s\n",
        SHOW(cls->get_name()));

    // Reset class-specific state
    used_field_names.clear();
    f_ob_state.used_ids.clear();
    static_field_name_gen.reset();
    simple_field_name_gen.reset();

    walk_hierarchy(cls, &publicFieldVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking public supers\n");
    walk_hierarchy(cls, &allFieldVisitor, HierarchyDirection::VisitSubClasses);
    TRACE(OBFUSCATE, 3, "Finished walking all subclasses\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    if (operate_on_ifields)
      obfuscate_elems(FieldRenamingContext(cls->get_ifields(), used_field_names,
          &simple_field_name_gen, false),
        &f_ob_state);
    if (operate_on_sfields)
      obfuscate_elems(FieldRenamingContext(cls->get_sfields(), used_field_names,
          &static_field_name_gen, false),
        &f_ob_state);
    TRACE(OBFUSCATE, 2, "Finished obfuscating publics\n");

    // Obfu private fields
    used_field_names.clear();
    walk_hierarchy(cls, &publicFieldVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking public supers\n");
    walk_hierarchy(cls, &allFieldVisitor, HierarchyDirection::VisitNeither);

    // Keep this for all public ids in the class (they shouldn't conflict)
    if (operate_on_ifields)
      obfuscate_elems(FieldRenamingContext(cls->get_ifields(), used_field_names,
          &static_field_name_gen, true),
        &f_ob_state);
    if (operate_on_sfields)
      obfuscate_elems(FieldRenamingContext(cls->get_sfields(), used_field_names,
          &static_field_name_gen, true),
        &f_ob_state);

    // Make sure to bind the new names otherwise not all generators will assign
    // names to the members
    simple_field_name_gen.bind_names();
    static_field_name_gen.bind_names();
    TRACE(OBFUSCATE, 2, "Finished obfuscating privates\n");

    /* Rename methods here in a similar manner, but be careful of more things
       - Interface implementors
       - library boundaries (taken care of)
       - overrides (same method name in same hierarchy)
    */

    // TODO: extend implementation for vmethods
    // Class-specific method state
    used_method_names.clear();
    m_ob_state.used_ids.clear();
    simple_method_name_gen.reset();

    // need to change hierarchy walks
    walk_hierarchy(cls, &publicFieldVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking public supers\n");
    walk_hierarchy(cls, &allFieldVisitor, HierarchyDirection::VisitSubClasses);
    TRACE(OBFUSCATE, 3, "Finished walking all subclasses\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    if (operate_on_dmethods)
      obfuscate_elems(MethodRenamingContext(cls->get_dmethods(), used_method_names,
          &simple_method_name_gen, false),
        &m_ob_state);
    if (operate_on_vmethods)
      obfuscate_elems(MethodRenamingContext(cls->get_vmethods(), used_method_names,
          &simple_method_name_gen, false),
        &m_ob_state);
    TRACE(OBFUSCATE, 2, "Finished obfuscating publics\n");

    // Obfu private methods
    used_method_names.clear();
    walk_hierarchy(cls, &publicFieldVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 3, "Finished walking public supers\n");
    walk_hierarchy(cls, &allFieldVisitor, HierarchyDirection::VisitNeither);

    if (operate_on_dmethods)
      obfuscate_elems(MethodRenamingContext(cls->get_dmethods(), used_method_names,
          &simple_method_name_gen, true),
        &m_ob_state);
    if (operate_on_vmethods)
      obfuscate_elems(MethodRenamingContext(cls->get_vmethods(), used_method_names,
          &simple_method_name_gen, true),
        &m_ob_state);

    // Make sure to bind the new names otherwise not all generators will assign
    // names to the members
    simple_method_name_gen.bind_names();
  }
  f_ob_state.name_mapping->print_elements();
  m_ob_state.name_mapping->print_elements();

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
      rewrite_if_field_instr(f_ob_state, instr);
      rewrite_if_method_instr(m_ob_state, instr);
    });

  TRACE(OBFUSCATE, 2, "Finished transforming refs\n");

  // Apply new names, recording what we're changing
  f_ob_state.commit_renamings_to_dex();
  m_ob_state.commit_renamings_to_dex();
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

void redex::ObfuscatePass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  obfuscate(scope, &cfg.get_proguard_map());
  post_dexen_changes(scope, stores);
}

static redex::ObfuscatePass s_pass;
