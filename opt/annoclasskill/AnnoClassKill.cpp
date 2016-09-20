/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AnnoClassKill.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Walkers.h"

typedef std::unordered_set<DexClass*> class_set_t;

int clear_annotation_references(Scope& scope, class_set_t& deadclasses) {
  /*
   * These annotations show in up method parameter annotations,
   * but they are still unused.  We have to visit all the
   * method param annotations and remove them.
   */
  int cleared_annos = 0;
  walk_methods(
    scope,
    [&](DexMethod* method) {
      /* Parameter annotations... */
      auto pas = method->get_param_anno();
      if (pas == nullptr) return;
      bool clear_pas = true;
      for (auto pa : *pas) {
        DexAnnotationSet* aset = pa.second;
        if (aset->size() == 0) continue;
        auto& annos = aset->get_annotations();
        auto iter = annos.begin();
        while (iter != annos.end()) {
          auto tokill = iter;
          DexAnnotation* da = *iter++;
          DexClass* clazz = type_class(da->type());
          if (deadclasses.count(clazz)) {
            annos.erase(tokill);
            delete da;
          }
        }
        if (aset->size() != 0) {
          clear_pas = false;
        }
      }
      if (clear_pas) {
        for (auto pa : *pas) {
          delete pa.second;
        }
        pas->clear();
        cleared_annos++;
        TRACE(CLASSKILL,
              5,
              "Cleared parameter annotations for method %s\n",
              SHOW(method));
      }
    });
  return cleared_annos;
}

/**
 * Remove annotation classes that are marked explicitly with a removable
 * annotation (specified as "kill_annos" in config).  Facebook uses these to
 * remove DI binding annotations.
 */
void kill_annotation_classes(
  Scope& scope,
  const std::unordered_set<DexType*>& kill_annos
) {
  // Determine which annotation classes are removable.
  class_set_t bannotations;
  for (auto clazz : scope) {
    if (!(clazz->get_access() & DexAccessFlags::ACC_ANNOTATION)) continue;
    auto aset = clazz->get_anno_set();
    if (aset == nullptr) continue;
    auto& annos = aset->get_annotations();
    for (auto anno : annos) {
      if (kill_annos.count(anno->type())) {
        bannotations.insert(clazz);
        TRACE(CLASSKILL, 5, "removable annotation class %s\n",
              SHOW(clazz->get_type()));
      }
    }
  }

  // Annotation classes referenced explicitly can't be removed.
  walk_code(
    scope,
    [](DexMethod*) { return true; },
    [&](DexMethod* meth, DexCode& code) {
      auto opcodes = code.get_instructions();
      for (const auto& opcode : opcodes) {
        if (opcode->has_types()) {
          auto typeop = static_cast<DexOpcodeType*>(opcode);
          auto dtexclude = typeop->get_type();
          DexClass* exclude = type_class(dtexclude);
          if (exclude != nullptr && bannotations.count(exclude)) {
            bannotations.erase(exclude);
          }
        }
      }
    });

  // Do the removal.
  int annotations_removed_count = 0;
  if (bannotations.size()) {
    // We have some annotations we can kill.  First let's clear all annotation
    // references to the classes.
    annotations_removed_count =
        clear_annotation_references(scope, bannotations);
    scope.erase(
      std::remove_if(
        scope.begin(), scope.end(),
        [&](DexClass* cls) { return bannotations.count(cls); }),
      scope.end());
  }
  TRACE(CLASSKILL, 1,
          "Annotation classes removed %lu\n",
          bannotations.size());
  TRACE(CLASSKILL, 1,
          "Method param annotations removed %d\n",
          annotations_removed_count);
}

std::unordered_set<DexType*> get_kill_annos(
  const std::vector<std::string>& kill
) {
  std::unordered_set<DexType*> kill_annos;
  try {
    for (auto const& config_anno : kill) {
      DexType* anno = DexType::get_type(config_anno.c_str());
      if (anno) {
        TRACE(CLASSKILL, 2, "kill anno: %s\n", SHOW(anno));
        kill_annos.insert(anno);
      }
    }
  } catch (const std::exception&) {
    // Swallow exception if the config doesn't have any annos.
  }
  return kill_annos;
}

void AnnoClassKillPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto kill_annos = get_kill_annos(m_kill_annos);
  kill_annotation_classes(scope, kill_annos);
  post_dexen_changes(scope, stores);
}

static AnnoClassKillPass s_pass;
