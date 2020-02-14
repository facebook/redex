/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeErasureInterDexPlugin.h"

#include "DexUtil.h"
#include "IRCode.h"
#include "ModelMerger.h"

namespace {

DexType* get_model_root(
    const std::unordered_map<const DexType*, ModelSpec>& root_to_model_specs,
    const DexClass* clazz) {
  const DexClass* super_cls = clazz;
  std::unordered_set<DexType*> super_types;

  while (super_cls) {
    super_types.emplace(super_cls->get_type());
    DexType* super_type = super_cls->get_super_class();
    if (root_to_model_specs.count(super_type)) {

      // Check this is not part of the exclude group.
      auto& exclude_types = root_to_model_specs.at(super_type).exclude_types;
      for (auto& type : super_types) {
        if (exclude_types.count(type)) {
          return nullptr;
        }
      }

      return super_type;
    }

    super_cls = type_class_internal(super_type);
  }

  return nullptr;
}

} // namespace

/*
 * We consider as mergeable a class that is a subclass of one of the roots
 * and not part of the exclude group.
 */
bool TypeErasureInterDexPlugin::is_mergeable(const DexClass* clazz) {
  // Double checking it is not a class we previously generated.
  if (m_generated_types.count(clazz)) {
    return false;
  }

  return get_model_root(m_root_to_model_spec, clazz) != nullptr;
}

/*
 * Skipping all the classes that we might potentially merge.
 *
 */
bool TypeErasureInterDexPlugin::should_skip_class(const DexClass* clazz) {
  if (is_mergeable(clazz)) {
    m_mergeables_skipped.emplace(clazz->get_type());
    return true;
  }

  return false;
}

/*
 * Not relocating methods of all the classes that we have generated.
 */
bool TypeErasureInterDexPlugin::should_not_relocate_methods_of_class(
    const DexClass* clazz) {
  return !!m_generated_types.count(clazz);
}

/*
 * NOTE: We want to "make room" for all the classes that might
 * potentially get merged here.
 *
 * For each mergeable that gets instantiated, we keep track of all
 * the method / field refs. The later logic in additional_classes
 * will make sure that those classes actually get added.
 */
void TypeErasureInterDexPlugin::gather_refs(
    const interdex::DexInfo& dex_info,
    const DexClass* cls,
    std::vector<DexMethodRef*>& mrefs,
    std::vector<DexFieldRef*>& frefs,
    std::vector<DexType*>& trefs,
    std::vector<DexClass*>* erased_classes,
    bool should_not_relocate_methods_of_class) {

  std::vector<DexMethod*> methods = cls->get_dmethods();
  const std::vector<DexMethod*>& vmethods = cls->get_vmethods();
  methods.insert(methods.end(), vmethods.begin(), vmethods.end());

  std::unordered_set<DexMethodRef*> mrefs_set(mrefs.begin(), mrefs.end());
  for (const DexMethod* method : methods) {
    auto code = method->get_code();
    if (!code) {
      continue;
    }

    for (const auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (insn->opcode() != OPCODE_NEW_INSTANCE) {
        continue;
      }

      DexType* type = insn->get_type();
      if (type == nullptr || type_class(type) == nullptr) {
        continue;
      }

      if (!is_mergeable(type_class(type))) {
        continue;
      }

      // Keep track of the classes the mergeable was
      // instantiated in.
      m_mergeable_to_cls[type].emplace(cls);

      if (m_mergeables_selected.count(type)) {
        continue;
      }

      // Keep track of the class we want to add.
      DexType* root = get_model_root(m_root_to_model_spec, type_class(type));
      m_current_mergeables[root].emplace(type);
      m_mergeables_selected.emplace(type);
      // Keep track of the class the mergeable was instantiated in first.
      m_cls_to_mergeables[cls->get_type()].emplace(type);

      DexClass* current_cls = type_class(type);
      if (erased_classes) erased_classes->push_back(current_cls);
      current_cls->gather_methods(mrefs);
      current_cls->gather_fields(frefs);
      current_cls->gather_types(trefs);

      // As we might end up referencing some of the virtual parent methods,
      // include them too here.
      for (const auto& vmethod : current_cls->get_vmethods()) {
        auto vscope = m_type_system->find_virtual_scope(vmethod);
        auto top_def = vscope->methods[0].first;
        auto top_def_cls = type_class(top_def->get_class());
        if (!is_interface(top_def_cls) && mrefs_set.count(top_def) == 0) {
          mrefs.push_back(top_def);
          mrefs_set.emplace(top_def);
        }
      }
    }
  }
}

/**
 * Filter mergeables that were instantiated in classes that did not end
 * up in this dex.
 */
void TypeErasureInterDexPlugin::filter_extra_mergeables(
    const DexClasses& classes) {

  std::unordered_set<DexClass*> classes_set(classes.begin(), classes.end());
  for (const auto& pair : m_cls_to_mergeables) {
    DexClass* cls = type_class(pair.first);
    if (classes_set.count(cls) == 0) {
      TRACE(TERA, 5, "[interdex] Class %s did not end up in the dex",
            SHOW(cls));

      const auto& mergeables = pair.second;
      for (const auto& mergeable : mergeables) {
        always_assert(m_mergeable_to_cls[mergeable].size() == 1);

        // Remove it from the list of current mergeables.
        // the list of skpped classes only grows and nothing gets erased
        // at the end the missing types to emit (leftover_classes) is
        // the difference between skipped and selected types.
        m_mergeables_selected.erase(mergeable);

        DexType* root =
            get_model_root(m_root_to_model_spec, type_class(mergeable));
        m_current_mergeables[root].erase(mergeable);
      }
    }
  }

  // Cleanup.
  m_mergeable_to_cls.clear();
  m_cls_to_mergeables.clear();
}

DexClasses TypeErasureInterDexPlugin::additional_classes(
    const DexClassesVector& outdex, const DexClasses& classes) {
  DexClasses additional_classes;

  if (m_current_mergeables.empty()) {
    // No mergeables here.
    return additional_classes;
  }

  // Make sure we are only tracking mergeables that were instantiated in
  // the classes that end up in this dex.
  filter_extra_mergeables(classes);

  for (const auto& current_mergeables : m_current_mergeables) {
    const DexType* root = current_mergeables.first;
    auto& mergeables = current_mergeables.second;
    always_assert(m_root_to_model_spec.count(root));
    ModelSpec& model_spec = m_root_to_model_spec.at(root);
    // Still keeping all the classes around, since we are leaving it up to
    // RemoveUnreachable to remove them.
    for (auto& mergeable : mergeables) {
      additional_classes.emplace_back(type_class(mergeable));
    }

    TypeSystem type_system(m_scope);
    auto model =
        Model::build_model(m_scope, model_spec, mergeables, type_system);
    model.update_redex_stats(m_mgr);

    ModelMerger mm;
    DexStoresVector empty_stores;
    auto merger_classes = mm.merge_model(m_scope, empty_stores, model);
    mm.update_redex_stats(model.get_class_name_prefix(), m_mgr);
    m_generated_types.insert(merger_classes.begin(), merger_classes.end());
    additional_classes.insert(additional_classes.end(), merger_classes.begin(),
                              merger_classes.end());
  }

  m_current_mergeables.clear();

  return additional_classes;
}

DexClasses TypeErasureInterDexPlugin::leftover_classes() {
  DexClasses additional_classes;

  for (DexType* type : m_mergeables_skipped) {
    if (m_mergeables_selected.count(type) == 0) {
      DexClass* cls = type_class(type);
      always_assert(cls);

      additional_classes.emplace_back(cls);
      m_mergeables_selected.emplace(type);
    }
  }

  // This output needs to be stable.
  std::sort(additional_classes.begin(), additional_classes.end(),
            compare_dexclasses);
  return additional_classes;
}

void TypeErasureInterDexPlugin::cleanup(const std::vector<DexClass*>&) {
  always_assert_log(m_mergeables_skipped.size() <= m_mergeables_selected.size(),
                    "Not all skipped mergeables were selected!");
}
