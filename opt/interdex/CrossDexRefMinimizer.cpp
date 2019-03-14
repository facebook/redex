/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cmath>
#include <numeric>

#include "CrossDexRefMinimizer.h"
#include "DexUtil.h"

namespace interdex {

template <class Value, size_t N>
std::string format_infrequent_refs_array(const std::array<Value, N>& array) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < N; ++i) {
    if (i > 0) {
      ss << ",";
    }
    ss << array[i];
  }
  ss << "]";
  return ss.str();
}

uint64_t CrossDexRefMinimizer::ClassInfo::get_primary_priority_denominator()
    const {
  always_assert(refs_weight >= applied_refs_weight);
  always_assert(refs_weight >=
                std::accumulate(infrequent_refs_weight.begin(),
                                infrequent_refs_weight.end(), 0UL,
                                [](uint32_t x, uint32_t y) {
                                  return static_cast<uint64_t>(x) +
                                         static_cast<uint64_t>(y);
                                }));
  uint64_t unapplied_refs_weight = refs_weight - applied_refs_weight;
  int64_t denominator = static_cast<int64_t>(
      std::min(unapplied_refs_weight,
               static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
  // Discount unapplied refs by infrequent refs,
  // with highest discount for most infrequent refs.
  // TODO: Try some other variations.
  for (size_t i = 0; i < infrequent_refs_weight.size(); ++i) {
    denominator -= infrequent_refs_weight[i] / (i + 1);
  }
  return static_cast<uint64_t>(std::max(denominator, INT64_C(1)));
}

uint64_t CrossDexRefMinimizer::ClassInfo::get_priority() const {
  uint64_t nominator = applied_refs_weight;
  uint64_t denominator = get_primary_priority_denominator();
  uint64_t primary_priority = (nominator << 20) / denominator;
  primary_priority = std::min(primary_priority, (UINT64_C(1) << 40) - 1);

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
    affected_class_info.applied_refs_weight += delta.applied_refs_weight;
    for (size_t i = 0; i < INFREQUENT_REFS_COUNT; ++i) {
      affected_class_info.infrequent_refs_weight[i] +=
          delta.infrequent_refs_weight[i];
    }

    const auto priority = affected_class_info.get_priority();
    m_prioritized_classes.update_priority(affected_class, priority);
    TRACE(
        IDEX, 5,
        "[dex ordering] Reprioritized class {%s} with priority %016lx; "
        "index %u; %u (delta %d) applied refs weight, %s (delta %s) infrequent "
        "refs weights, %u total refs\n",
        SHOW(affected_class), priority, affected_class_info.index,
        affected_class_info.applied_refs_weight, delta.applied_refs_weight,
        format_infrequent_refs_array(affected_class_info.infrequent_refs_weight)
            .c_str(),
        format_infrequent_refs_array(delta.infrequent_refs_weight).c_str(),
        affected_class_info.refs.size());
  }
}

void CrossDexRefMinimizer::insert(DexClass* cls) {
  always_assert(m_class_infos.count(cls) == 0);
  ++m_stats.classes;
  CrossDexRefMinimizer::ClassInfo& class_info =
      m_class_infos
          .insert({cls, CrossDexRefMinimizer::ClassInfo(m_next_index++)})
          .first->second;

  // Collect all relevant references that contribute to cross-dex metadata
  // entries.
  // We don't bother with protos and type_lists, as they are directly related
  // to method refs (I tried, didn't help).
  std::vector<DexMethodRef*> method_refs;
  std::vector<DexFieldRef*> field_refs;
  std::vector<DexType*> types;
  std::vector<DexString*> strings;
  cls->gather_methods(method_refs);
  sort_unique(method_refs);
  cls->gather_fields(field_refs);
  sort_unique(field_refs);
  cls->gather_types(types);
  sort_unique(types);
  cls->gather_strings(strings);
  sort_unique(strings);
  auto& refs = class_info.refs;
  uint64_t refs_weight = 0;
  refs.reserve(method_refs.size() + field_refs.size() + types.size() +
               strings.size());

  // Record all references with a particular weight.
  // The weights are somewhat arbitrary, but they were chosen after trying many
  // different values and observing the effect on APK size.
  // TODO: Try some other variations.
  for (auto mref : method_refs) {
    uint32_t weight = m_config.method_ref_weight;
    refs.emplace_back(mref, weight);
    refs_weight += weight;
  }
  for (auto type : types) {
    uint32_t weight = m_config.type_ref_weight;
    refs.emplace_back(type, weight);
    refs_weight += weight;
  }
  for (auto string : strings) {
    uint32_t weight = m_config.string_ref_weight;
    refs.emplace_back(string, weight);
    refs_weight += weight;
  }
  for (auto fref : field_refs) {
    uint32_t weight = m_config.field_ref_weight;
    refs.emplace_back(fref, weight);
    refs_weight += weight;
  }
  class_info.refs_weight = refs_weight;

  std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>
      affected_classes;
  for (const std::pair<void*, uint32_t>& p : refs) {
    void* ref = p.first;
    uint32_t weight = p.second;
    auto& classes = m_ref_classes[ref];
    size_t frequency = classes.size();
    // We record the need to undo (subtract weight of) a previously claimed
    // infrequent ref. The actual undoing happens later in
    // reprioritize.
    if (frequency > 0 && frequency <= INFREQUENT_REFS_COUNT) {
      for (DexClass* affected_class : classes) {
        always_assert(affected_class != cls);
        affected_classes[affected_class]
            .infrequent_refs_weight[frequency - 1] -= weight;
      }
    }
    ++frequency;
    // We are recording a new infrequent unapplied ref, if any.
    // This happens immediately for the to be inserted class cls,
    // so that it can be used right away by the upcoming
    // class_info.get_priority() call, while all other change requests happen
    // later in reprioritize.
    if (frequency <= INFREQUENT_REFS_COUNT) {
      for (DexClass* affected_class : classes) {
        affected_classes[affected_class]
            .infrequent_refs_weight[frequency - 1] += weight;
      }
      class_info.infrequent_refs_weight[frequency - 1] += weight;
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
        "%s infrequent refs weights, %u total refs\n",
        SHOW(cls), priority, class_info.index,
        format_infrequent_refs_array(class_info.infrequent_refs_weight).c_str(),
        refs.size());
  reprioritize(affected_classes);
}

bool CrossDexRefMinimizer::empty() const {
  return m_prioritized_classes.empty();
}

DexClass* CrossDexRefMinimizer::front() const {
  return m_prioritized_classes.front();
}

DexClass* CrossDexRefMinimizer::worst(bool generated) {
  auto max_it = m_class_infos.end();
  uint64_t max_value = 0;

  for (auto it = m_class_infos.begin(); it != m_class_infos.end(); ++it) {
    // If requested, let's skip generated classes, as they tend to be not stable
    // and may cause drastic build-over-build changes.
    if (it->first->rstate.is_generated() != generated) {
      continue;
    }

    const CrossDexRefMinimizer::ClassInfo& class_info = it->second;
    uint64_t value = class_info.get_primary_priority_denominator();

    // Prefer the largest denominator
    if (value < max_value) {
      continue;
    }

    // If equal, prefer the class that was inserted earlier (smaller index) to
    // make things deterministic.
    if (value == max_value && max_it != m_class_infos.end() &&
        class_info.index > max_it->second.index) {
      continue;
    }

    max_it = it;
    max_value = value;
  }

  if (max_it == m_class_infos.end()) {
    return nullptr;
  }

  TRACE(IDEX, 3,
        "[dex ordering] Picked worst class {%s} with priority %016lx; "
        "index %u; %u applied refs weight, %s infrequent refs weights, %u "
        "total refs\n",
        SHOW(max_it->first), max_it->second.get_priority(),
        max_it->second.index, max_it->second.applied_refs_weight,
        format_infrequent_refs_array(max_it->second.infrequent_refs_weight)
            .c_str(),
        max_it->second.refs.size());
  m_stats.worst_classes.emplace_back(max_it->first, max_value);
  return max_it->first;
}

DexClass* CrossDexRefMinimizer::worst() {
  always_assert(!m_class_infos.empty());
  // We prefer to find a class that is not generated. Only when such a class
  // doesn't exist (because all classes are generated), then we pick the worst
  // generated class.
  DexClass* cls = worst(/* generated */ false);
  if (cls == nullptr) {
    cls = worst(/* generated */ true);
  }
  always_assert(cls != nullptr);
  return cls;
}

void CrossDexRefMinimizer::erase(DexClass* cls, bool emitted, bool reset) {
  m_prioritized_classes.erase(cls);
  auto class_info_it = m_class_infos.find(cls);
  always_assert(class_info_it != m_class_infos.end());
  const CrossDexRefMinimizer::ClassInfo& class_info = class_info_it->second;
  TRACE(IDEX, 3,
        "[dex ordering] Processing class {%s} with priority %016lx; "
        "index %u; %u applied refs weight, %s infrequent refs weights, %u "
        "total refs; emitted %d\n",
        SHOW(cls), class_info.get_priority(), class_info.index,
        class_info.applied_refs_weight,
        format_infrequent_refs_array(class_info.infrequent_refs_weight).c_str(),
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
  for (const std::pair<void*, uint32_t>& p : refs) {
    void* ref = p.first;
    uint32_t weight = p.second;
    auto& classes = m_ref_classes.at(ref);
    size_t frequency = classes.size();
    always_assert(frequency > 0);
    const auto erased = classes.erase(cls);
    always_assert(erased);
    if (frequency <= INFREQUENT_REFS_COUNT) {
      for (DexClass* affected_class : classes) {
        affected_classes[affected_class]
            .infrequent_refs_weight[frequency - 1] -= weight;
      }
    }
    --frequency;
    if (frequency > 0 && frequency <= INFREQUENT_REFS_COUNT) {
      for (DexClass* affected_class : classes) {
        affected_classes[affected_class]
            .infrequent_refs_weight[frequency - 1] += weight;
      }
    }

    if (!emitted) {
      continue;
    }
    if (m_applied_refs.count(ref) != 0) {
      continue;
    }
    m_applied_refs.emplace(ref);
    for (DexClass* affected_class : classes) {
      affected_classes[affected_class].applied_refs_weight += weight;
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
      reset_class_info.applied_refs_weight = 0;
      const auto priority = reset_class_info.get_priority();
      m_prioritized_classes.insert(reset_class, priority);
      always_assert(reset_class_info.applied_refs_weight == 0);
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
