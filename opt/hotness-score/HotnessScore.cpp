/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "HotnessScore.h"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Debug.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Walkers.h"

namespace {

template <typename T>
using ref_freq = std::unordered_map</*referenced*/ T,
                                    /*freq*/ int>;

// Reference freq information per class and per caller.
template <typename T>
using ref_info =
    std::unordered_map<DexClass*,
                       std::pair</*from all callers*/ ref_freq<T>,
                                 /*per caller stat*/
                                 std::unordered_map<DexMethod*, ref_freq<T>>>>;

const std::array<const char*, 4> kHotNames{{"HOT", "WARM", "MILD", "COLD"}};

// Dump helper for the references (C++11 doesn't have generic lambdas in C++14)
// Header: Trial RefKind ClassName CallerName Hot? RefName RefFreq
template <typename T, typename U>
void dump_reference_stats(const int trial,
                          const char* kind,
                          const ref_info<T>& info,
                          U hot_set_test) {
  // Per class, per caller, per ref...
  auto print = [&](const DexClass* cls,
                   const std::string& caller,
                   const ref_freq<T>& ref_stats) {
    for (const auto& per_ref : ref_stats) {
      TRACE(HOTNESS,
            5,
            "%d\t%s\t%s\t%s\t%s\t%s\t%d\n",
            trial,
            kind,
            SHOW(cls),
            caller.c_str(),
            kHotNames[hot_set_test(cls, per_ref.first)],
            SHOW(per_ref.first),
            per_ref.second);
    }
  };
  for (const auto& per_class : info) {
    const auto& cls = per_class.first;
    print(cls, "<all-callers>", per_class.second.first);
    for (const auto& per_caller : per_class.second.second) {
      print(cls, show(per_caller.first), per_caller.second);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Hotness score, H(%), for a given class in the cold start class set, is
// defined as follows:
//
//       # of "hot" references (method/type/field) from the given class
//  H = ----------------------------------------------------------------
//       # of all references (method/type/field) from the given class
//
// This stat pass dumps the scores via TRACE. The output is mostly divided by
// tabs so that Excel can easily process the data.
////////////////////////////////////////////////////////////////////////////////
void report_hotness_scores(
    const int trial,
    const std::array<std::vector<DexClass*>, 3>& coldstart_scopes) {
  std::array<std::unordered_set<const DexClass*>, 3> coldstart_sets;
  for (size_t i = 0; i < coldstart_scopes.size(); ++i) {
    for (const auto& cls : coldstart_scopes[i]) {
      coldstart_sets[i].insert(cls);
    }
  }

  // 0: Hot, 1: Warm, 2: Mild, 3: Cold (not part of cold start list), 4: Self
  auto hot_degree = [&coldstart_sets](const DexClass* caller_cls,
                                      const DexClass* ref_cls) -> size_t {
    if (caller_cls == ref_cls) {
      return coldstart_sets.size() + 1;
    }
    for (size_t i = 0; i < coldstart_sets.size(); ++i) {
      if (coldstart_sets[i].find(ref_cls) != end(coldstart_sets[i])) {
        return i;
      }
    }
    return coldstart_sets.size();
  };

  ref_info<DexMethod*> method_refs;
  ref_info<DexField*> field_refs;
  ref_info<DexType*> type_refs;

  struct Score {
    std::array<int, 5> degree;
    int total;
  };
  std::unordered_map<DexClass*, Score> all_scores;
  std::unordered_map<DexClass*, Score> method_scores;

  auto collect_stats = [&](DexMethod* caller, IRInstruction* opcode) {
    DexClass* cls = type_class(caller->get_class());
    DexClass* ref_cls = nullptr;
    // Working with refs is ugly. Time to do refactoring?
    if (opcode->has_method()) {
      auto resolved = resolve_method(opcode->get_method(), MethodSearch::Any);
      if (resolved == nullptr) {
        return;
      }
      ref_cls = type_class(resolved->get_class());
      if (ref_cls == nullptr || ref_cls->is_external()) {
        return;
      }
      method_refs[cls].first[resolved]++;
      method_refs[cls].second[caller][resolved]++;
    } else if (opcode->has_field()) {
      auto resolved = resolve_field(opcode->get_field(), FieldSearch::Any);
      if (resolved == nullptr) {
        return;
      }
      ref_cls = type_class(resolved->get_class());
      if (ref_cls == nullptr || ref_cls->is_external()) {
        return;
      }
      field_refs[cls].first[resolved]++;
      field_refs[cls].second[caller][resolved]++;
    } else if (opcode->has_type()) {
      ref_cls = type_class(opcode->get_type());
      if (ref_cls == nullptr || ref_cls->is_external()) {
        return;
      }
      type_refs[cls].first[opcode->get_type()]++;
      type_refs[cls].second[caller][opcode->get_type()]++;
    } else {
      return;
    }

    all_scores[cls].degree[hot_degree(cls, ref_cls)]++;
    all_scores[cls].total++;
    if (opcode->has_method()) {
      method_scores[cls].degree[hot_degree(cls, ref_cls)]++;
      method_scores[cls].total++;
    }
  };

  // Hotness scores for Hot/Warm/Mild sets.
  for (size_t i = 0; i < coldstart_scopes.size(); ++i) {
    all_scores.clear();
    method_scores.clear();

    walk::opcodes(
        coldstart_scopes[i], [](DexMethod*) { return true; }, collect_stats);

    // Printing the stats. The header is:
    //  Trial Set TAG Class
    //  #HotRef #WarmRef #MildRef #ColdRef #SelfRef #TotalRef Hotness
    for (auto&& p : {std::make_pair(all_scores, "ALL"),
                     std::make_pair(method_scores, "METHOD_ONLY")}) {
      for (const auto& q : p.first) {
        TRACE(HOTNESS,
              5,
              "%d\t%s_SET\t%s\t%s\t"
              "%d\t%d\t%d\t%d\t%d\t%d\t%.3lf\n",
              trial,
              kHotNames[i],
              p.second,
              SHOW(q.first),
              q.second.degree[0],
              q.second.degree[1],
              q.second.degree[2],
              q.second.degree[3],
              q.second.degree[4],
              q.second.total,
              double(q.second.degree[0]) / double(q.second.total));
      }
    }
  }

  dump_reference_stats(trial,
                       "METHOD",
                       method_refs,
                       [&](const DexClass* caller_cls, const DexMethod* ref) {
                         return hot_degree(caller_cls,
                                           type_class(ref->get_class()));
                       });
  dump_reference_stats(trial,
                       "FIELD",
                       field_refs,
                       [&](const DexClass* caller_cls, const DexField* ref) {
                         return hot_degree(caller_cls,
                                           type_class(ref->get_class()));
                       });
  dump_reference_stats(trial,
                       "TYPE",
                       type_refs,
                       [&](const DexClass* caller_cls, const DexType* ref) {
                         return hot_degree(caller_cls, type_class(ref));
                       });
}

} // namespace

// I hate it. We don't yet have a nice way to get the number of repetition
// how many this pass is being executed.
static int g_trial = 1;

void HotnessScorePass::run_pass(DexStoresVector& /*stores*/,
                                ConfigFiles& cfg,
                                PassManager& /*mgr*/) {
  const auto& coldstart_classes = cfg.get_coldstart_classes();
  if (coldstart_classes.size() == 0) {
    TRACE(HOTNESS, 1, "Empty or no coldstart_classes file\n");
    return;
  }

  // Partitioning the cold start set to three DexClass sets (hot, warm, mild).
  int hotness = 0;
  int cold_class_count = 0;
  std::array<std::vector<DexClass*>, 3> scopes;
  for (const auto& cls_name : coldstart_classes) {
    if (cls_name == m_warm_marker) {
      hotness = 1;
      TRACE(
          HOTNESS, 5, "%d\tPARTITION\tMARKER\t%s\n", g_trial, cls_name.c_str());
    } else if (cls_name == m_mild_marker) {
      hotness = 2;
      TRACE(
          HOTNESS, 5, "%d\tPARTITION\tMARKER\t%s\n", g_trial, cls_name.c_str());
    } else {
      ++cold_class_count;
      auto type = DexType::get_type(cls_name.c_str());
      if (type) {
        DexClass* cls = type_class(type);
        if (cls != nullptr && !cls->is_external()) {
          TRACE(HOTNESS,
                5,
                "%d\tPARTITION\t%d\t%s\tFOUND\t%s_SET\n",
                g_trial,
                cold_class_count,
                cls_name.c_str(),
                kHotNames[hotness]);
          scopes[hotness].push_back(cls);
        } else {
          TRACE(HOTNESS,
                5,
                "%d\tPARTITION\t%d\t%s\tFOUND_BUT_EXTERNAL\n",
                g_trial,
                cold_class_count,
                cls_name.c_str());
        }
      } else {
        TRACE(HOTNESS,
              5,
              "%d\tPARTITION\t%d\t%s\tNOT_FOUND\n",
              g_trial,
              cold_class_count,
              cls_name.c_str());
      }
    }
  }

  const auto total = scopes[0].size() + scopes[1].size() + scopes[2].size();
  TRACE(HOTNESS,
        1,
        "Loaded %d cold start class names from the file; found %lu classes\n",
        cold_class_count,
        total);
  TRACE(HOTNESS,
        1,
        "Loaded %lu hot classes (%.lf%%)\n",
        scopes[0].size(),
        (scopes[0].size() * 100.) / total);
  TRACE(HOTNESS,
        1,
        "Loaded %lu warm classes (%.lf%%)\n",
        scopes[1].size(),
        (scopes[0].size() * 100.) / total);
  TRACE(HOTNESS,
        1,
        "Loaded %lu mild classes (%.lf%%)\n",
        scopes[2].size(),
        (scopes[2].size() * 100.) / total);

  report_hotness_scores(g_trial, scopes);
  g_trial++;
}

static HotnessScorePass s_pass;
