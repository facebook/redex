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

// Renames a field in the Dex
bool rename_field(DexField* field,
    const std::string& new_name) {
  // To be done. This is a dummy definition.
  if (keep(field)) {
   if (!allowobfuscation(field)) {
     return false;
   }
  }
  DexFieldRef ref;
  std::string old_name = field->get_name()->c_str();
  ref.name = DexString::make_string(new_name);
  TRACE(OBFUSCATE, 1, "Renaming the field %s to %s\n", old_name.c_str(),
                      ref.name->c_str());
  field->change(ref);
  return true;
}

// Obfuscates a list of fields
// fields - the fields to be obfuscated
// name_mapping - the mapping of descriptor -> new name -> old name
// reverse_mapping - will be replaced by something that can hold names that
//   we're thinking of changing to but haven't committed to yet
// ids_to_avoid - the names that we can't change and should avoid colliding with
// used_ids - the names that we've used in assignment, gets updated as we use
//            more names
void obfuscate_fields(const std::list<DexField*>& fields,
    NameMapping* name_mapping,
    NameMapping* reverse_mapping,
    const unordered_set<std::string>& ids_to_avoid,
    unordered_set<std::string>* used_ids,
    bool operateOnPrivates) {
  IdFactory idf(ids_to_avoid, *used_ids);
  for (DexField* field : fields) {
    if(!should_rename_field(field) ||
        (operateOnPrivates && !is_private(field)) ||
        (!operateOnPrivates && is_private(field))) {
      TRACE(OBFUSCATE, 2, "Ignoring field %s because we shouldn't rename it\n",
        field->get_name()->c_str());
      continue;
    }
    std::string new_name(idf.next_name());
    TRACE(OBFUSCATE, 1, "Renaming field %s to %s\n",
      field->get_name()->c_str(), new_name.c_str());
    used_ids->insert(new_name);
    rename_field(field, new_name);
    // Do something here to record the name we're trying to give along
    // with a reverse-mapping
    //(*name_mapping)[" "][field->get_name()->c_str()] = new_name;
    //(*reverse_mapping)[" "][new_name] = field->get_name()->c_str();
    // put something in the name mapping
    // name_mapping[field->descriptor()->] FIGURE OUT HOW TO GET DESCRIPTOR
  }
}

void rename_method(DexMethod* method) {
  // To be done. This is a dummy definition.
  if (keep(method)) {
   if (!allowobfuscation(method)) {
     return;
   }
  }
  DexMethodRef ref;
  std::string old_name = method->get_name()->c_str();
  ref.name = DexString::make_string(old_name + "_renamed");
  method->change(ref);
}

void obfuscate_methods(const std::list<DexMethod*>& methods,
                       ProguardMap* pg_map) {
  for (DexMethod* method : methods) {
    rename_method(method);
  }
}

// Obfuscate methods and fields, updating the ProGuard
// map approriately to refelct renamings.
void obfuscate(Scope& classes, ProguardMap* pg_map) {
  // descriptor - [oldname - newname]
  NameMapping name_mapping;
  // descriptor - [newname - oldname]
  NameMapping reverse_mapping;

  unordered_set<std::string> ids_to_avoid;
  unordered_set<std::string> used_ids;
  FieldVisitor publicVisitor(&ids_to_avoid,
    ClassVisitor::VisitFilter::NonPrivateOnly);
  FieldVisitor allVisitor(&ids_to_avoid, ClassVisitor::VisitFilter::All);
  FieldVisitor privateVisitor(&ids_to_avoid,
    ClassVisitor::VisitFilter::PrivateOnly);

  for (DexClass* cls : classes) {
    if (cls->is_external()) {
      TRACE(OBFUSCATE, 1, "Not renaming the members of external class %s\n",
        cls->get_name()->c_str());
      continue;
    }
    TRACE(OBFUSCATE, 1, "Renaming the members of class %s\n",
      cls->get_name()->c_str());

    // Obfu public fields
    ids_to_avoid.clear();
    used_ids.clear();
    walk_hierarchy(cls, &publicVisitor, HierarchyDirection::VisitSuperClasses);
    TRACE(OBFUSCATE, 2, "Finished walking hierarchies 1.1\n");
    walk_hierarchy(cls, &allVisitor, HierarchyDirection::VisitSubClasses);
    TRACE(OBFUSCATE, 2, "Finished walking hierarchies 1.2\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    obfuscate_fields(cls->get_ifields(),
      &name_mapping,
      &reverse_mapping,
      ids_to_avoid,
      &used_ids,
      false);
    obfuscate_fields(cls->get_sfields(),
      &name_mapping,
      &reverse_mapping,
      ids_to_avoid,
      &used_ids,
      false);
    TRACE(OBFUSCATE, 1, "Finished obfuscating publics\n");

    // Obfu private fields
    ids_to_avoid.clear();
    used_ids.clear();
    walk_hierarchy(cls, &publicVisitor, HierarchyDirection::VisitSuperClasses);
    walk_hierarchy(cls, &allVisitor, HierarchyDirection::VisitNeither);
    TRACE(OBFUSCATE, 2, "Finished walking hierarchies 2\n");
    // Keep this for all public ids in the class (they shouldn't conflict)
    obfuscate_fields(cls->get_ifields(),
      &name_mapping,
      &reverse_mapping,
      ids_to_avoid,
      &used_ids,
      true);
    obfuscate_fields(cls->get_sfields(),
      &name_mapping,
      &reverse_mapping,
      ids_to_avoid,
      &used_ids,
      true);
    TRACE(OBFUSCATE, 1, "Finished obfuscating privates\n");
    // Dex entries need to be sorted and we may have disrupted that.
    cls->get_ifields().sort(compare_dexfields);
    cls->get_sfields().sort(compare_dexfields);
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
