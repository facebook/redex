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

namespace {

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
template <class T, class R, class K>
void obfuscate_elems(const RenamingContext<T>& context,
    DexElemManager<T, R, K>& name_mapping) {
  for (T elem : context.elems) {
    if (!context.can_rename_elem(elem)) {
      TRACE(OBFUSCATE, 4, "Ignoring member %s because we shouldn't rename it\n",
          SHOW(elem->get_name()));
      continue;
    }
    context.name_gen.find_new_name(name_mapping[elem]);
  }
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

// Obfuscate methods and fields, updating the ProGuard
// map approriately to reflect renamings.
void obfuscate(Scope& classes, ProguardMap* pg_map) {
  DexFieldManager field_name_manager(new_dex_field_manager());
  DexMethodManager method_name_manager(new_dex_method_manager());

  TRACE(OBFUSCATE, 2, "Starting obfuscation of fields and methods\n");
  for (DexClass* cls : classes) {
    always_assert_log(!cls->is_external(),
        "Shouldn't rename members of external classes.");
    // First check if we will do anything on this class
    bool operate_on_ifields = contains_renamable_elem(cls->get_ifields());
    bool operate_on_sfields = contains_renamable_elem(cls->get_sfields());
    bool operate_on_dmethods = contains_renamable_elem(cls->get_dmethods());
    bool operate_on_vmethods = false;//contains_renamable_elem(cls->get_vmethods());
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
      if (operate_on_ifields)
        obfuscate_elems(FieldRenamingContext(cls->get_ifields(),
            f_ob_state.ids_to_avoid,
            simple_name_generator, false),
          field_name_manager);
      if (operate_on_sfields)
        obfuscate_elems(FieldRenamingContext(cls->get_sfields(),
            f_ob_state.ids_to_avoid,
            static_name_generator, false),
          field_name_manager);
      TRACE(OBFUSCATE, 2, "Finished obfuscating publics\n");

      // Obfu private fields
      f_ob_state.ids_to_avoid.clear();
      f_ob_state.populate_ids_to_avoid(cls, field_name_manager, false);

      // Keep this for all public ids in the class (they shouldn't conflict)
      if (operate_on_ifields)
        obfuscate_elems(FieldRenamingContext(cls->get_ifields(),
            f_ob_state.ids_to_avoid,
            simple_name_generator, true),
          field_name_manager);
      if (operate_on_sfields)
        obfuscate_elems(FieldRenamingContext(cls->get_sfields(),
            f_ob_state.ids_to_avoid,
            static_name_generator, true),
          field_name_manager);

      // Make sure to bind the new names otherwise not all generators will assign
      // names to the members
      static_name_generator.bind_names();
      TRACE(OBFUSCATE, 2, "Finished obfuscating privates\n");
    }

    // =========== Obfuscate Methods Below ==========
    if (operate_on_vmethods || operate_on_dmethods) {
      MethodObfuscationState m_ob_state;
      SimpleNameGenerator<DexMethod*> simple_name_gen(m_ob_state.ids_to_avoid,
          m_ob_state.used_ids);

      m_ob_state.populate_ids_to_avoid(cls, method_name_manager, true);

      // Keep this for all public ids in the class (they shouldn't conflict)
      if (operate_on_dmethods)
        obfuscate_elems(MethodRenamingContext(cls->get_dmethods(),
            m_ob_state.ids_to_avoid, simple_name_gen, false),
          method_name_manager);
      if (operate_on_vmethods)
        obfuscate_elems(MethodRenamingContext(cls->get_vmethods(),
            m_ob_state.ids_to_avoid, simple_name_gen, false),
          method_name_manager);
      TRACE(OBFUSCATE, 2, "Finished obfuscating publics\n");

      // Obfu private methods
      m_ob_state.ids_to_avoid.clear();
      m_ob_state.populate_ids_to_avoid(cls, method_name_manager, false);

      if (operate_on_dmethods)
        obfuscate_elems(MethodRenamingContext(cls->get_dmethods(),
            m_ob_state.ids_to_avoid, simple_name_gen, true),
          method_name_manager);
      if (operate_on_vmethods)
        obfuscate_elems(MethodRenamingContext(cls->get_vmethods(),
            m_ob_state.ids_to_avoid, simple_name_gen, true),
          method_name_manager);
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
  field_name_manager.commit_renamings_to_dex();
  method_name_manager.commit_renamings_to_dex();
  sort_members(classes);
}

} // end namespace

void ObfuscatePass::run_pass(DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& /*unused*/) {
  auto scope = build_class_scope(stores);
  obfuscate(scope, &cfg.get_proguard_map());
}

static ObfuscatePass s_pass;
