/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cinttypes>
#include <cmath>
#include <numeric>

#include "CrossDexRefMinimizer.h"
#include "DexUtil.h"
#include "Show.h"
#include "Trace.h"
#include "WorkQueue.h"

namespace cross_dex_ref_minimizer {

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

void CrossDexRefMinimizer::ClassDiffSet::insert(DexClass* value) {
  always_assert(!m_diff.count(value));
  m_base->insert(value);
}

void CrossDexRefMinimizer::ClassDiffSet::erase(DexClass* value) {
  always_assert(m_base->count(value));
  m_diff.insert(value);
  if (m_diff.size() >= (m_base->size() + 1) / 2) {
    // When diff set size becomes significant, create a new *shared* base set,
    // so that operations such as enumeration over all elements retain their
    // expected complexity.
    auto new_base = std::make_shared<Repr>(size());
    for (auto* cls : *m_base) {
      if (!m_diff.count(cls)) {
        new_base->insert(cls);
      }
    }
    m_base = std::move(new_base);
    m_diff.clear();
  }
}

void CrossDexRefMinimizer::ClassDiffSet::compact() {
  for (auto* cls : m_diff) {
    m_base->erase(cls);
  }
  m_diff.clear();
}

void CrossDexRefMinimizer::reprioritize(
    const std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>&
        affected_classes) {
  TRACE(IDEX, 4, "[dex ordering] Reprioritizing %zu classes",
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
        "[dex ordering] Reprioritized class {%s} with priority %016" PRIu64
        "; index %u; %" PRIu64 " (delta %" PRId64
        ") applied refs weight, %s (delta %s) infrequent refs weights, %zu "
        "total refs",
        SHOW(affected_class), priority, affected_class_info.index,
        affected_class_info.applied_refs_weight, delta.applied_refs_weight,
        format_infrequent_refs_array(affected_class_info.infrequent_refs_weight)
            .c_str(),
        format_infrequent_refs_array(delta.infrequent_refs_weight).c_str(),
        affected_class_info.refs->size());
  }
}

void CrossDexRefMinimizer::sample(DexClass* cls) {
  const auto& cls_refs = m_cache->get(cls);
  auto increment = [&ref_counts = m_ref_counts,
                    &max_ref_count = m_max_ref_count](const void* ref) {
    size_t& count = ref_counts[ref];
    if (count < std::numeric_limits<size_t>::max() && ++count > max_ref_count) {
      max_ref_count = count;
    }
  };
  for (auto ref : cls_refs.method_refs) {
    increment(ref);
  }
  for (auto ref : cls_refs.field_refs) {
    increment(ref);
  }
  for (auto ref : cls_refs.types) {
    increment(ref);
  }
  for (auto ref : cls_refs.strings) {
    increment(ref);
  }

  if (m_json_classes) {
    Json::Value json_class;
    json_class["method_refs"] = m_json_methods.get(cls_refs.method_refs);
    json_class["field_refs"] = m_json_fields.get(cls_refs.field_refs);
    json_class["types"] = m_json_types.get(cls_refs.types);
    json_class["strings"] = m_json_strings.get(cls_refs.strings);
    json_class["is_generated"] = cls->rstate.is_generated();
    json_class["insert_index"] = -1;
    (*m_json_classes)[get_json_class_index(cls)] = json_class;
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
  const auto& cls_refs = m_cache->get(cls);

  auto& refs = *class_info.refs;
  refs.reserve(cls_refs.method_refs.size() + cls_refs.field_refs.size() +
               cls_refs.types.size() + cls_refs.strings.size());
  uint64_t& refs_weight = class_info.refs_weight;
  uint64_t& seed_weight = class_info.seed_weight;

  auto add_weight = [&ref_counts = m_ref_counts,
                     max_ref_count = m_max_ref_count, &refs, &refs_weight,
                     &seed_weight](const void* ref, size_t item_weight,
                                   size_t item_seed_weight) {
    auto it = ref_counts.find(ref);
    auto ref_count = it == ref_counts.end() ? 1 : it->second;
    double frequency = ref_count * 1.0 / max_ref_count;
    // We skip reference that...
    // - only ever appear once (those won't help with prioritization), and
    // - and those which appear extremely frequently (and are therefore likely
    //   to be referenced by every dex anyway)
    bool skipping = ref_count == 1 || frequency > (1.0 / 8);
    TRACE(IDEX, 6, "[dex ordering] %zu/%zu = %lf %s", ref_count, max_ref_count,
          frequency, skipping ? "(skipping)" : "");
    if (!skipping) {
      refs.emplace_back(ref, item_weight);
      refs_weight += item_weight;
      seed_weight += item_seed_weight;
    }
  };

  // Record all references with a particular weight.
  // The weights are somewhat arbitrary, but they were chosen after trying many
  // different values and observing the effect on APK size.
  // We discount references that occur in many classes.
  // TODO: Try some other variations.
  for (auto mref : cls_refs.method_refs) {
    add_weight(mref, m_config.method_ref_weight, m_config.method_seed_weight);
  }
  for (auto type : cls_refs.types) {
    add_weight(type, m_config.type_ref_weight, m_config.type_seed_weight);
  }
  for (auto string : cls_refs.strings) {
    add_weight(string, m_config.string_ref_weight, m_config.string_seed_weight);
  }
  for (auto fref : cls_refs.field_refs) {
    add_weight(fref, m_config.field_ref_weight, m_config.field_seed_weight);
  }

  std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>
      affected_classes;
  for (const auto& p : refs) {
    auto ref = p.first;
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
    classes.insert(cls);
  }
  const auto priority = class_info.get_priority();
  m_prioritized_classes.insert(cls, priority);
  TRACE(IDEX, 4,
        "[dex ordering] Inserting class {%s} with priority %016" PRIu64
        "; index %u; %s infrequent refs weights, %zu total refs",
        SHOW(cls), priority, class_info.index,
        format_infrequent_refs_array(class_info.infrequent_refs_weight).c_str(),
        refs.size());
  if (m_json_classes) {
    (*m_json_classes)[get_json_class_index(cls)]["insert_index"] =
        class_info.index;
  }
  reprioritize(affected_classes);
}

bool CrossDexRefMinimizer::empty() const {
  return m_prioritized_classes.empty();
}

DexClass* CrossDexRefMinimizer::front() const {
  return m_prioritized_classes.front();
}

std::vector<DexClass*> CrossDexRefMinimizer::worst(size_t count,
                                                   bool include_generated) {
  std::map<uint64_t, std::map<size_t, DexClass*, std::greater<size_t>>>
      selected;
  size_t selected_count{0};

  for (auto it = m_class_infos.begin(); it != m_class_infos.end(); ++it) {
    const CrossDexRefMinimizer::ClassInfo& class_info = it->second;
    uint64_t value = class_info.seed_weight;

    if (it->first->rstate.is_generated()) {
      if (!include_generated) {
        continue;
      }
      // We still prefer to find a class that is not generated, as they tend to
      // be not stable and may cause drastic build-over-build changes. Thus we
      // cut the seed weight for generated classes in half.
      value /= 2;
    }

    if (selected_count >= count && selected.begin()->first > value) {
      continue;
    }

    selected[value][class_info.index] = it->first;
    selected_count++;

    // If equal, prefer the class that was inserted earlier (smaller index) to
    // make things deterministic.
    while (selected_count > count) {
      auto selected_it = selected.begin();
      auto selected_ordered_it = selected_it->second.begin();
      selected_it->second.erase(selected_ordered_it);
      if (selected_it->second.empty()) {
        selected.erase(selected_it);
      }
      selected_count--;
    }
  }

  std::ostringstream ss;
  std::vector<DexClass*> classes;
  for (auto rit = selected.rbegin(); rit != selected.rend(); rit++) {
    auto& [value, selected_ordered] = *rit;
    for (auto [index, cls] : selected_ordered) {
      if (traceEnabled(IDEX, 3)) {
        ss << "Effective seed " << value << ": {" << SHOW(cls) << "}; index "
           << index << "\n";
      }
      classes.push_back(cls);
    }
  }
  always_assert(classes.size() == selected_count);
  TRACE(IDEX, 3, "[dex ordering] Picked %zu <= %zu worst classes:\n%s",
        selected_count, count, ss.str().c_str());
  return classes;
}

DexClass* CrossDexRefMinimizer::worst() {
  always_assert(!m_class_infos.empty());
  // We prefer to find a class that is not generated. Only when such a class
  // doesn't exist (because all classes are generated), then we pick the worst
  // generated class.
  auto classes = worst(1, /* include_generated */ false);
  if (classes.empty()) {
    classes = worst(1, /* include_generated */ true);
  }
  always_assert(!classes.empty());
  always_assert(classes.front() != nullptr);
  return classes.front();
}

size_t CrossDexRefMinimizer::erase(DexClass* cls, bool emitted, bool reset) {
  std::unordered_map<DexClass*, ClassInfo>::const_iterator class_info_it =
      m_class_infos.end();
  if (cls) {
    m_prioritized_classes.erase(cls);
    class_info_it = m_class_infos.find(cls);
    always_assert(class_info_it != m_class_infos.end());
    const auto& class_info = class_info_it->second;
    if (m_stats.seed_classes.empty() || m_applied_refs.empty()) {
      m_stats.seed_classes.emplace_back(cls, class_info.seed_weight);
    }
    TRACE(
        IDEX, 3,
        "[dex ordering] Processing class {%s} with priority %016" PRIu64
        "; index %u; %" PRIu64
        " applied refs weight, %s infrequent refs weights, %zu total refs; "
        "emitted %d",
        SHOW(cls), class_info.get_priority(), class_info.index,
        class_info.applied_refs_weight,
        format_infrequent_refs_array(class_info.infrequent_refs_weight).c_str(),
        class_info.refs->size(), emitted);
  } else {
    always_assert(!emitted);
  }

  // Updating m_applied_refs and m_ref_classes,
  // and gathering information on how this affects other classes

  if (reset) {
    TRACE(IDEX, 3, "[dex ordering] Reset");
    ++m_stats.resets;
    m_applied_refs.clear();
  }

  std::unordered_map<DexClass*, CrossDexRefMinimizer::ClassInfoDelta>
      affected_classes;
  size_t old_applied_refs = m_applied_refs.size();
  if (class_info_it != m_class_infos.end()) {
    const auto& class_info = class_info_it->second;
    const auto& refs = *class_info.refs;
    for (const auto& p : refs) {
      auto ref = p.first;
      uint32_t weight = p.second;
      auto classes_it = m_ref_classes.find(ref);
      always_assert(classes_it != m_ref_classes.end());
      auto& classes = classes_it->second;
      size_t frequency = classes.size();
      always_assert(frequency > 0);
      classes.erase(cls);
      if (frequency <= INFREQUENT_REFS_COUNT) {
        for (DexClass* affected_class : classes) {
          affected_classes[affected_class]
              .infrequent_refs_weight[frequency - 1] -= weight;
        }
      }
      --frequency;
      if (frequency == 0) {
        m_ref_classes.erase(classes_it);
      } else if (frequency <= INFREQUENT_REFS_COUNT) {
        for (DexClass* affected_class : classes) {
          affected_classes[affected_class]
              .infrequent_refs_weight[frequency - 1] += weight;
        }
      }

      if (!emitted) {
        continue;
      }
      if (!m_applied_refs.insert(ref).second) {
        continue;
      }
      if (frequency == 0) {
        continue;
      }
      for (DexClass* affected_class : classes) {
        affected_classes[affected_class].applied_refs_weight += weight;
      }
    }

    // Updating m_class_infos and m_prioritized_classes

    m_class_infos.erase(class_info_it);
  }

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
    TRACE(IDEX, 4, "[dex ordering] %zu + %zu = %zu applied refs",
          old_applied_refs, m_applied_refs.size() - old_applied_refs,
          m_applied_refs.size());
  }
  reprioritize(affected_classes);
  return m_applied_refs.size() - old_applied_refs;
}

size_t CrossDexRefMinimizer::get_unapplied_refs(DexClass* cls) const {
  auto it = m_class_infos.find(cls);
  if (it == m_class_infos.end()) {
    return 0;
  }
  size_t unapplied_refs{0};
  const auto& refs = *it->second.refs;
  for (auto& p : refs) {
    if (!m_applied_refs.count(p.first)) {
      unapplied_refs++;
    }
  }
  return unapplied_refs;
}

void CrossDexRefMinimizer::compact() {
  for (auto&& [ref, classes] : m_ref_classes) {
    classes.compact();
  }
}

double CrossDexRefMinimizer::get_remaining_difficulty() const {
  // As a proxy for the remaining difficulty, we compute the sum over the
  // inverse squares of how many classes reference each remaining reference.
  // (Why? Unclear, but it seems to work well in practice.) The following does
  // this computation in a way that results in a high precision and is
  // deterministic using floating-point values.
  std::unordered_map<size_t, size_t> counts;
  for (auto& p : m_ref_classes) {
    counts[p.second.size()]++;
  }
  std::vector<double> summands;
  summands.reserve(counts.size());
  std::transform(counts.begin(), counts.end(), std::back_inserter(summands),
                 [](auto& p) { return p.second * 1.0 / (p.first * p.first); });
  std::sort(summands.begin(), summands.end());
  return std::accumulate(summands.begin(), summands.end(), 0.0);
}

std::string CrossDexRefMinimizer::get_json_class_index(DexClass* cls) {
  return m_json_types.get(cls->get_type());
}

Json::Value CrossDexRefMinimizer::get_json_class_indices(
    const std::vector<DexClass*>& classes) {
  std::vector<DexType*> types;
  types.reserve(classes.size());
  for (auto* cls : classes) {
    types.push_back(cls->get_type());
  }
  return m_json_types.get(types);
}

Json::Value CrossDexRefMinimizer::get_json_mapping() {
  // These could be further nested into a ref-specific path,
  // but it just makes the mapping more annoying to use.
  Json::Value res = Json::objectValue;
  m_json_methods.get_mapping(&res);
  m_json_fields.get_mapping(&res);
  m_json_types.get_mapping(&res);
  m_json_strings.get_mapping(&res);
  return res;
}

} // namespace cross_dex_ref_minimizer
