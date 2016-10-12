/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ObfuscateUtils.h"
#include "Trace.h"

std::string IdFactory::next_name()  {
  std::string res = "";
  do {
    int ctr_cpy = ctr;
    res.clear();
    while(ctr_cpy > 0) {
      res += get_ident(ctr_cpy % kMaxIdentChar);
      ctr_cpy /= kMaxIdentChar;
    }
    ctr += 1;
    TRACE(OBFUSCATE, 2, "IdFactory looking for a name, trying: %s\n",
      res.c_str());
  } while(ids_to_avoid.count(res) > 0 || used_ids.count(res) > 0);
  return res;
}

void FieldVisitor::visit(DexClass* cls) {
  for (auto field : cls->get_ifields())
    if (should_rename_field(field))
      visit_field(field);
  for (auto field : cls->get_sfields())
    if (should_rename_field(field))
      visit_field(field);
}

void walk_hierarchy(DexClass* cls,
    ClassVisitor* visitor,
    HierarchyDirection h_dir) {
  if (!cls) return;
  visitor->visit(cls);

  if (h_dir & HierarchyDirection::VisitSuperClasses) {
    auto clazz = cls;
    while (clazz) {
      TRACE(OBFUSCATE, 2, "Visiting %s\n", clazz->c_str());
      visitor->visit(clazz);
      if (clazz->get_super_class()) {
        clazz = type_class(clazz->get_super_class());
      } else {
        break;
      }
    }
  }

  if (h_dir & HierarchyDirection::VisitSubClasses) {
    for(auto subcls_type : get_children(cls->get_type())) {
      walk_hierarchy(type_class(subcls_type), visitor,
        HierarchyDirection::VisitSubClasses);
    }
  }
}
