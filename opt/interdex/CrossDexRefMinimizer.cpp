/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CrossDexRefMinimizer.h"
#include "DexUtil.h"

namespace interdex {

uint64_t CrossDexRefMinimizer::ClassInfo::get_priority() const {
  always_assert(refs.size() >= applied_refs);
  always_assert(refs.size() >= singleton_refs);
  uint64_t unapplied_refs = refs.size() - applied_refs;

  uint64_t nominator = applied_refs + singleton_refs;
  uint64_t denominator = unapplied_refs < singleton_refs
                             ? 1
                             : (unapplied_refs - singleton_refs + 1);
  uint64_t primary_priority = (nominator << 20) / denominator;
  // primary_priority should comfortably fit into 40 bits

  // Note that locator.h imposes a limit of (1<<6)-1 dexes, which in fact
  // implies a much lower limit of around 1<<22 classes.
  always_assert(index < (1 << 24));
  uint32_t secondary_priority = 0xFFFFFF - index;

  // The combined priority is a composite of the primary and secondary
  // priority, where the primary priority is using the top 40 bits, and the
  // secondary priority the low 24 bits.

  return (primary_priority << 24) | secondary_priority;
}

void CrossDexRefMinimizer::reprioritize(
    const std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>&
        affected_classes) {
  TRACE(IDEX, 4, "[dex ordering] Reprioritizing %u classes\n",
        affected_classes.size());
  for (auto p : affected_classes) {
    ++m_stats.reprioritizations;
    DexClass* affected_class = p.first;
    CrossDexRefMinimizer::ClassInfoDelta& delta = p.second;
    CrossDexRefMinimizer::ClassInfo& affected_class_info =
        m_class_infos.at(affected_class);
    affected_class_info.applied_refs += delta.applied_refs;
    affected_class_info.singleton_refs += delta.singleton_refs;

    const auto priority = affected_class_info.get_priority();
    m_prioritized_classes.update_priority(affected_class, priority);
    TRACE(IDEX, 5,
          "[dex ordering] Reprioritized class {%s} with priority %016lx; "
          "index %u; %u (delta %d) applied refs, %u (delta %d) singleton "
          "unapplied refs, %u total refs\n",
          SHOW(affected_class), priority, affected_class_info.index,
          affected_class_info.applied_refs, delta.applied_refs,
          affected_class_info.singleton_refs, delta.singleton_refs,
          affected_class_info.refs.size());
  }
}

void CrossDexRefMinimizer::insert(DexClass* cls) {
  always_assert(m_class_infos.count(cls) == 0);
  ++m_stats.classes;
  CrossDexRefMinimizer::ClassInfo& class_info =
      m_class_infos
          .insert({cls, CrossDexRefMinimizer::ClassInfo(m_class_infos.size())})
          .first->second;
  std::vector<DexMethodRef*> method_refs;
  std::vector<DexFieldRef*> field_refs;
  std::vector<DexType*> types;
  std::vector<DexString*> strings;
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  cls->gather_types(types);
  cls->gather_strings(strings);
  auto& refs = class_info.refs;
  refs.reserve(method_refs.size() + field_refs.size() + types.size() +
               strings.size());
  refs.insert(refs.end(), method_refs.begin(), method_refs.end());
  refs.insert(refs.end(), field_refs.begin(), field_refs.end());
  refs.insert(refs.end(), types.begin(), types.end());
  refs.insert(refs.end(), strings.begin(), strings.end());

  sort_unique(refs);
  std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>
      affected_classes;
  for (void* ref : refs) {
    auto& classes = m_ref_classes[ref];
    switch (classes.size()) {
    case 1:
      // we record the need to undo (decrement) a previously claimed singleton
      // ref. The actual undoing happens later in reprioritize.
      affected_classes[*classes.begin()].singleton_refs--;
      break;
    case 0:
      // we are recording a new singleton ref, immediately. So that it can be
      // used right away by the upcoming class_info.get_priority() call.
      class_info.singleton_refs++;
      break;
    }
    // There's an implicit invariant that class_info and the keys of
    // affected_classes are disjoint, so we are not going to reprioritize
    // the class that we are adding here.
    classes.emplace(cls);
  }
  const auto priority = class_info.get_priority();
  m_prioritized_classes.insert(cls, priority);
  TRACE(IDEX, 4,
        "[dex ordering] Inserting class {%s} with priority %016lx; index %u; "
        "%u singleton refs, %u total refs\n",
        SHOW(cls), priority, class_info.index, class_info.singleton_refs,
        refs.size());
  reprioritize(affected_classes);
}

bool CrossDexRefMinimizer::empty() const {
  return m_prioritized_classes.empty();
}

DexClass* CrossDexRefMinimizer::front() const {
  return m_prioritized_classes.front();
}

void CrossDexRefMinimizer::erase(DexClass* cls, bool emitted, bool reset) {
  m_prioritized_classes.erase(cls);
  auto class_info_it = m_class_infos.find(cls);
  always_assert(class_info_it != m_class_infos.end());
  const CrossDexRefMinimizer::ClassInfo& class_info = class_info_it->second;
  TRACE(IDEX, 3,
        "[dex ordering] Processing class {%s} with priority %016lx; "
        "index %u; %u applied refs, %u singleton refs, %u total refs; "
        "emitted %d\n",
        SHOW(cls), class_info.get_priority(), class_info.index,
        class_info.applied_refs, class_info.singleton_refs,
        class_info.refs.size(), emitted);

  // Updating m_applied_refs and m_ref_classes,
  // and gathering information on how this affects other classes

  if (reset) {
    TRACE(IDEX, 3, "[dex ordering] Reset\n");
    ++m_stats.resets;
    m_applied_refs.clear();
  }

  std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>
      affected_classes;
  const auto& refs = class_info.refs;
  size_t old_applied_refs = m_applied_refs.size();
  for (void* ref : refs) {
    auto& classes = m_ref_classes.at(ref);
    const auto erased = classes.erase(cls);
    always_assert(erased);
    if (classes.size() == 1) {
      affected_classes[*classes.begin()].singleton_refs++;
    }

    if (!emitted) {
      continue;
    }
    if (m_applied_refs.count(ref) != 0) {
      continue;
    }
    m_applied_refs.emplace(ref);
    for (DexClass* affected_class : classes) {
      affected_classes[affected_class].applied_refs++;
    }
  }

  // Updating m_class_infos and m_prioritized_classes

  m_class_infos.erase(class_info_it);
  always_assert(m_class_infos.count(cls) == 0);

  if (reset) {
    m_prioritized_classes.clear();
    for (auto it = m_class_infos.begin(); it != m_class_infos.end(); ++it) {
      DexClass* reset_class = it->first;
      CrossDexRefMinimizer::ClassInfo& reset_class_info = it->second;
      reset_class_info.applied_refs = 0;
      const auto priority = reset_class_info.get_priority();
      m_prioritized_classes.insert(reset_class, priority);
      always_assert(reset_class_info.applied_refs == 0);
    }
  }
  if (emitted) {
    TRACE(IDEX, 4, "[dex ordering] %u + %u = %u applied refs\n",
          old_applied_refs, m_applied_refs.size() - old_applied_refs,
          m_applied_refs.size());
  }
  reprioritize(affected_classes);
}

} // namespace interdex
