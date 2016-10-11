/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Obfuscate.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ProguardMap.h"
#include "ReachableClasses.h"
#include "Trace.h"

void rename_field(DexField* field) {
  // To be done. This is a dummy definition.
  if (keep(field)) {
   if (!allowobfuscation(field)) {
     return;
   }
  }
  DexFieldRef ref;
  std::string old_name = field->get_name()->c_str();
  ref.name = DexString::make_string(old_name + "_renamed");
  TRACE(OBFUSCATE, 1, "Renaming the field %s to %s\n", old_name.c_str(),
                      ref.name->c_str());
  field->change(ref);
}

void obfuscate_fields(const std::list<DexField*>& fields, ProguardMap* pg_map) {
  for (DexField* field : fields) {
    rename_field(field);
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

void obfuscate_class(const DexClass* cls, ProguardMap* pg_map) {
  if (!cls->is_external()) {
    obfuscate_fields(cls->get_ifields(), pg_map);
    obfuscate_fields(cls->get_sfields(), pg_map);
    obfuscate_methods(cls->get_vmethods(), pg_map);
    obfuscate_methods(cls->get_dmethods(), pg_map);
  }
}

// Obfuscate methods and fields, updating the ProGuard
// map approriately to refelct renamings.
void obfuscate(Scope& classes, ProguardMap* pg_map) {
  for (DexClass* cls : classes) {
    obfuscate_class(cls, pg_map);
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
