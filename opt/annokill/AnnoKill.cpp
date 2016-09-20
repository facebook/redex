/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AnnoKill.h"

#include <stdio.h>
#include <string>
#include <vector>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Walkers.h"

/*
 * Within FB current consensus is that annotations are degenerate and there
 * is discussion about eliminating them entirely.  For that reason we don't
 * make any attempts here to dynamically find uses of SYSTEM annotations,
 * we just black-list known paths.
 *
 * TODO(opensource): Consider re-working this for common annotation driven
 * cases, or a proper dynamic search for such cases.
 */

struct kill_counters {
  int annotations;
  int annotations_killed;
  int class_asets;
  int class_asets_cleared;
  int method_asets;
  int method_asets_cleared;
  int method_param_asets;
  int method_param_asets_cleared;
  int field_asets;
  int field_asets_cleared;
};

static kill_counters s_kcount;
/*
 * AnnotationDefaults are used by RUNTIME visible annotations.
 * Future optimization: We could build a dependency graph to
 * eliminate some cases.
 */
static const char* kAnnoDefault = "Ldalvik/annotation/AnnotationDefault;";

std::unordered_set<DexType*> get_blacklist(
  const std::vector<std::string>& config
) {
  std::unordered_set<DexType*> blacklist;
  for (auto const& config_blacklist : config) {
    DexType* entry = DexType::get_type(config_blacklist.c_str());
    if (entry) {
      TRACE(ANNO, 2, "blacklist class: %s\n", SHOW(entry));
      blacklist.insert(entry);
    }
  }
  return blacklist;
}

std::unordered_set<DexType*> get_annos(const std::vector<std::string>& remove) {
  std::unordered_set<DexType*> annos;
  for (auto const& config_anno : remove) {
    DexType* anno = DexType::get_type(config_anno.c_str());
    if (anno) {
      TRACE(ANNO, 2, "removable anno: %s\n", SHOW(anno));
      annos.insert(anno);
    }
  }
  return annos;
}

static void cleanup_aset(DexAnnotationSet* aset,
    std::unordered_set<DexType*>& removable_annos,
    bool remove_all_build_visible_annos,
    bool remove_all_system_visible_annos) {
  static DexType* annodefault = DexType::get_type(kAnnoDefault);

  s_kcount.annotations += aset->size();
  auto& annos = aset->get_annotations();
  auto iter = annos.begin();
  while (iter != annos.end()) {
    auto tokill = iter;
    DexAnnotation* da = *iter++;
    auto match_whitelist = removable_annos.count(da->type());
    auto match_build = remove_all_build_visible_annos && da->build_visible();
    auto match_system = remove_all_system_visible_annos && da->system_visible();
    if (!da->runtime_visible() && da->type() != annodefault
        && (match_build || match_system || match_whitelist)) {
      TRACE(ANNO, 2, "Killing annotation %s\n", SHOW(da));
      annos.erase(tokill);
      s_kcount.annotations_killed++;
      delete da;
    }
  }
}

/*
 * Return a subset of classes where each class does not contain
 * any class annotations which exist in blacklist_container_classes.
 */
std::vector<DexClass*> get_classes_not_containing_blacklisted_contained_annotations(
      const std::vector<DexClass*>& classes,
      std::unordered_set<DexType*>& blacklist_container_classes) {
  std::vector<DexClass*> non_container_classes;
  for (auto clazz : classes) {
    DexAnnotationSet* aset = clazz->get_anno_set();
    if (aset == nullptr) {
      non_container_classes.push_back(clazz);
    } else {
      s_kcount.class_asets++;
      bool isContained = false;
      auto& annos = aset->get_annotations();
      for (auto container_class:blacklist_container_classes) {
        for (auto anno:annos) {
          if (anno->type() == container_class) {
            TRACE(ANNO, 2,
                  "class %s has class annotation %s which is blacklisted\n",
                  SHOW(clazz->get_type()), SHOW(container_class));
            isContained = true;
            break;
          }
        }
        if (isContained) {
          break;
        }
      }
      if (!isContained) {
        non_container_classes.push_back(clazz);
      }
    }
  }
  TRACE(ANNO, 2, "non_container_classes.size() %d\n", non_container_classes.size());
  TRACE(ANNO, 2, "classes.size() %d\n", classes.size());
  return non_container_classes;
}


/*
 * Subclasses of TypeReference; use the system visible annotations
 * in order to imply the "type", rather than doing so explicitly.
 * This could be fixed by re-writing the constructor pattern and
 * explicitly feeding it.  However, the number of cases is very
 * small, so it's not worth doing.
 */
void kill_annotations(const std::vector<DexClass*>& classes,
    std::unordered_set<DexType*>& removable_annos,
    std::unordered_set<DexType*>& blacklist_classes,
    bool remove_build,
    bool remove_system) {
  for (auto clazz : classes) {
    DexAnnotationSet* aset = clazz->get_anno_set();
    if (aset == nullptr) continue;
    if (blacklist_classes.count(clazz->get_super_class()) > 0) {
      TRACE(ANNO, 3, "Skipping %s\n", show(clazz->get_type()).c_str());
      continue;
    }
    cleanup_aset(aset, removable_annos, remove_build, remove_system);
    if (aset->size() == 0) {
      TRACE(ANNO, 2, "Clearing annotation for class %s\n", SHOW(clazz));
      clazz->clear_annotations();
      s_kcount.class_asets_cleared++;
    }
  }
  walk_methods(
      classes,
      [&](DexMethod* method) {
        DexAnnotationSet* aset = method->get_anno_set();
        if (aset == nullptr) return;
        s_kcount.method_asets++;
        cleanup_aset(aset, removable_annos, remove_build, remove_system);
        if (aset->size() == 0) {
          TRACE(ANNO, 2, "Clearing annotations for method %s\n", SHOW(method));
          method->clear_annotations();
          s_kcount.method_asets_cleared++;
        }
      });
  walk_methods(classes,
      [&](DexMethod* method) {
        /* Parameter annotations... */
        auto pas = method->get_param_anno();
        if (pas == nullptr) return;
        s_kcount.method_param_asets += pas->size();
        bool clear_pas = true;
        for (auto pa : *pas) {
          DexAnnotationSet* aset = pa.second;
          if (aset->size() == 0) continue;
          cleanup_aset(aset, removable_annos, remove_build, remove_system);
          if (aset->size() == 0) continue;
          clear_pas = false;
        }
        if (clear_pas) {
          TRACE(ANNO, 2, "Clearing parameter annotations for method %s\n",
              SHOW(method));
          s_kcount.method_param_asets_cleared += pas->size();
          for (auto pa : *pas) {
            delete pa.second;
          }
          pas->clear();
        }
      });
  walk_fields(
      classes,
      [&](DexField* field) {
        DexAnnotationSet* aset = field->get_anno_set();
        if (aset == nullptr) return;
        s_kcount.field_asets++;
        cleanup_aset(aset, removable_annos, remove_build, remove_system);
        if (aset->size() == 0) {
          TRACE(ANNO, 2, "Clearing annotations for field %s\n", SHOW(field));
          field->clear_annotations();
          s_kcount.field_asets_cleared++;
        }
      });
}

void AnnoKillPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto removable_annos = get_annos(m_remove_annos);
  auto blacklist_classes = get_blacklist(m_blacklist);
  auto blacklist_classes_containing_class_annotations =
    get_blacklist(m_blacklist_classes_containing_class_annotations);

  /*
   * Pass one
   * Get the list of classes which do not contain any of the class annotations
   * in blacklist_of_contained_annotations
   */
  auto classes_not_containing_blacklisted_contained_annotations =
    get_classes_not_containing_blacklisted_contained_annotations(scope,
      blacklist_classes_containing_class_annotations);

  /*
   * Pass two
   * Kill the annotations in the list of
   * classes_not_containing_blacklisted_contained_annotations
   */
  kill_annotations(
    classes_not_containing_blacklisted_contained_annotations,
    removable_annos,
    blacklist_classes,
    m_remove_build,
    m_remove_system);
  TRACE(ANNO, 1, "AnnoKill report killed/total\n");
  TRACE(ANNO, 1,
      "Annotations: %d/%d\n",
      s_kcount.annotations_killed,
      s_kcount.annotations);
  TRACE(ANNO, 1,
      "Class Asets: %d/%d\n",
      s_kcount.class_asets_cleared,
      s_kcount.class_asets);
  TRACE(ANNO, 1,
      "Method Asets: %d/%d\n",
      s_kcount.method_asets_cleared,
      s_kcount.method_asets);
  TRACE(ANNO, 1,
      "MethodParam Asets: %d/%d\n",
      s_kcount.method_param_asets_cleared,
      s_kcount.method_param_asets);
  TRACE(ANNO, 1,
      "Field Asets: %d/%d\n",
      s_kcount.field_asets_cleared,
      s_kcount.field_asets);
}

static AnnoKillPass s_pass;
