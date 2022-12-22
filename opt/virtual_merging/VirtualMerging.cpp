/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * VirtualMergingPass removes virtual methods that override other virtual
 * methods, by merging them, under certain conditions.
 * - we omit virtual scopes that are involved in invoke-supers (this could be
 *   made less conservative)
 * - we omit virtual methods that might be involved in unresolved
 *   invoke-virtuals.
 * - of, course the usual `can_rename` and not `root` conditions.
 * - the overriding method must be inlinable into the overridden method (using
 *   standard inliner functionality)
 *
 * When overriding an abstract method, the body of the overriding method is
 * essentially just moved into the formerly abstract method, with a preceeding
 * cast-class instruction to make the type checker happy. (The actual
 * implementation is a special case of the below, using the inliner.)
 *
 * When overriding a non-abstract method, we first insert a prologue like the
 * following into the overridden method:
 *
 * instance-of               param0, DeclaringTypeOfOverridingMethod
 * move-result-pseudo        if_temp
 * if-nez                    if_temp, new_code
 * ... (old body)
 *
 * new_code:
 * cast-class                param0, DeclaringTypeOfOverridingMethod
 * move-result-pseudo-object temp
 * invoke-virtual            temp, param1, ..., paramN, OverridingMethod
 * move-result               result_temp
 * return                    result_temp
 *
 * And then we inline the invoke-virtual instruction. Details vary depending on
 * the whether the method actually has a result, and if so, what kind it is.
 */

#include "VirtualMerging.h"

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "CppUtil.h"
#include "DedupVirtualMethods.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Inliner.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "SourceBlocks.h"
#include "StlUtil.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_DEDUPPED_VIRTUAL_METHODS =
    "num_dedupped_virtual_methods";
constexpr const char* METRIC_INVOKE_SUPER_METHODS = "num_invoke_super_methods";
constexpr const char* METRIC_INVOKE_SUPER_UNRESOLVED_METHOD_REFS =
    "num_invoke_super_unresolved_methods_refs";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_SCOPES =
    "num_mergeable_virtual_scopes";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS =
    "num_mergeable_virtual_methods";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS_ANNOTATED_METHODS =
    "num_mergeable_virtual_method_annotated_methods";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_STORE_REFS =
    "num_mergeable_virtual_method_cross_store_refs";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_DEX_REFS =
    "num_mergeable_virtual_method_cross_dex_refs";
constexpr const char*
    METRIC_MERGEABLE_VIRTUAL_METHODS_INCONCRETE_OVERRIDDEN_METHODS =
        "num_mergeable_virtual_methods_inconcrete_overridden_methods";

constexpr const char* METRIC_MERGEABLE_PAIRS = "num_mergeable_pairs";
constexpr const char* METRIC_VIRTUAL_SCOPES_WITH_MERGEABLE_PAIRS =
    "num_virtual_scopes_with_mergeable_pairs";
constexpr const char* METRIC_UNABSTRACTED_METHODS = "num_unabstracted_methods";
constexpr const char* METRIC_UNINLINABLE_METHODS = "num_uninlinable_methods";
constexpr const char* METRIC_HUGE_METHODS = "num_huge_methods";
constexpr const char* METRIC_CALLER_SIZE_REMOVED_METHODS =
    "num_caller_size_removed_methods";
constexpr const char* METRIC_REMOVED_VIRTUAL_METHODS =
    "num_removed_virtual_methods";

constexpr size_t kAppear100Buckets = 10;

} // namespace

VirtualMerging::VirtualMerging(DexStoresVector& stores,
                               const inliner::InlinerConfig& inliner_config,
                               size_t max_overriding_method_instructions,
                               const api::AndroidSDK* min_sdk_api,
                               PerfConfig perf_config)
    : m_scope(build_class_scope(stores)),
      m_xstores(stores),
      m_xdexes(stores),
      m_type_system(m_scope),
      m_max_overriding_method_instructions(max_overriding_method_instructions),
      m_inliner_config(inliner_config),
      m_init_classes_with_side_effects(m_scope,
                                       /* create_init_class_insns */ false),
      m_perf_config(perf_config) {
  std::unordered_set<DexMethod*> no_default_inlinables;
  // disable shrinking options, minimizing initialization time
  m_inliner_config.shrinker = shrinker::ShrinkerConfig();
  int min_sdk = 0;
  m_inliner.reset(new MultiMethodInliner(
      m_scope, m_init_classes_with_side_effects, stores, no_default_inlinables,
      std::ref(m_concurrent_method_resolver), m_inliner_config, min_sdk,
      MultiMethodInlinerMode::None,
      /* true_virtual_callers */ {},
      /* inline_for_speed */ nullptr,
      /* bool analyze_and_prune_inits */ false,
      /* const std::unordered_set<DexMethodRef*>& configured_pure_methods */ {},
      min_sdk_api));
}
VirtualMerging::~VirtualMerging() {}

// Part 1: Identify which virtual methods get invoked via invoke-super --- we'll
// stay away from those virtual scopes
// TODO: Relax this. Some portions of those virtual scopes could still
// be handled
void VirtualMerging::find_unsupported_virtual_scopes() {
  ConcurrentSet<const DexMethod*> invoke_super_methods;
  ConcurrentSet<const DexMethodRef*> invoke_super_unresolved_method_refs;
  walk::parallel::opcodes(
      m_scope,
      [](const DexMethod*) { return true; },
      [&](const DexMethod*, IRInstruction* insn) {
        if (insn->opcode() == OPCODE_INVOKE_SUPER) {
          auto method_ref = insn->get_method();
          auto method = resolve_method(method_ref, MethodSearch::Virtual);
          if (method == nullptr) {
            invoke_super_unresolved_method_refs.insert(method_ref);
          } else {
            invoke_super_methods.insert(method);
          }
        }
      });

  m_stats.invoke_super_methods = invoke_super_methods.size();
  m_stats.invoke_super_unresolved_method_refs =
      invoke_super_unresolved_method_refs.size();

  for (auto method : invoke_super_methods) {
    m_unsupported_virtual_scopes.insert(
        m_type_system.find_virtual_scope(method));
  }

  for (auto method : invoke_super_unresolved_method_refs) {
    m_unsupported_named_protos[method->get_name()].insert(method->get_proto());
  }
}

// Part 2: Identify all overriding virtual methods which might potentially be
//         mergeable into other overridden virtual methods.
//         Group these methods by virtual scopes.
void VirtualMerging::compute_mergeable_scope_methods() {
  walk::parallel::methods(m_scope, [&](const DexMethod* overriding_method) {
    if (!overriding_method->is_virtual() || !overriding_method->is_concrete() ||
        is_native(overriding_method) || is_abstract(overriding_method)) {
      return;
    }
    always_assert(overriding_method->is_def());
    always_assert(overriding_method->is_concrete());
    always_assert(!overriding_method->is_external());
    always_assert(overriding_method->get_code());

    auto virtual_scope = m_type_system.find_virtual_scope(overriding_method);
    if (virtual_scope == nullptr) {
      TRACE(VM, 1, "[VM] virtual method {%s} has no virtual scope!",
            SHOW(overriding_method));
      return;
    }
    if (virtual_scope->type == overriding_method->get_class()) {
      // Actually, this method isn't overriding anything.
      return;
    }

    if (m_unsupported_virtual_scopes.count(virtual_scope)) {
      TRACE(VM, 5, "[VM] virtual method {%s} in an unsupported virtual scope",
            SHOW(overriding_method));
      return;
    }

    auto it = m_unsupported_named_protos.find(overriding_method->get_name());
    if (it != m_unsupported_named_protos.end() &&
        it->second.count(overriding_method->get_proto())) {
      // Never observed in practice, but I guess it might happen
      TRACE(VM, 1, "[VM] virtual method {%s} has unsupported name/proto",
            SHOW(overriding_method));
      return;
    }

    m_mergeable_scope_methods.update(
        virtual_scope,
        [&](const VirtualScope*,
            std::unordered_set<const DexMethod*>& s,
            bool /* exists */) { s.emplace(overriding_method); });
  });

  m_stats.mergeable_scope_methods = m_mergeable_scope_methods.size();
  for (auto& p : m_mergeable_scope_methods) {
    m_stats.mergeable_virtual_methods += p.second.size();
  }
}

namespace {

struct LocalStats {
  size_t overriding_methods{0};
  size_t cross_store_refs{0};
  size_t cross_dex_refs{0};
  size_t inconcrete_overridden_methods{0};
  size_t perf_skipped{0};
};

struct SimpleOrdering {
  using Map = std::unordered_map<const DexMethodRef*, double>;
  Map map;
  SimpleOrdering() = default;
  explicit SimpleOrdering(const method_profiles::MethodProfiles& profiles)
      : map(create_call_count_ordering(profiles)) {}

  double get_order(const DexMethodRef* m) const {
    auto it = map.find(m);
    if (it == map.end()) {
      return 0;
    }
    return it->second;
  }

  static Map create_call_count_ordering(
      const method_profiles::MethodProfiles& profiles) {
    std::unordered_map<const DexMethodRef*, std::pair<double, double>>
        call_counts;
    // Fill first part with cold-start.
    for (auto& p : profiles.method_stats(method_profiles::COLD_START)) {
      call_counts.emplace(p.first, std::make_pair(p.second.call_count, 0.0));
    }
    // Second part with maximum of other interactions.
    for (auto& p : profiles.all_interactions()) {
      for (auto& q : p.second) {
        auto& cc = call_counts[q.first].second;
        cc = std::max(cc, q.second.call_count);
      }
    }

    std::vector<const DexMethodRef*> profile_methods;
    profile_methods.reserve(call_counts.size());
    for (auto& p : call_counts) {
      profile_methods.push_back(p.first);
    }

    std::sort(profile_methods.begin(), profile_methods.end(),
              [&call_counts](const auto* lhs, const auto* rhs) {
                auto& lhs_p = call_counts.at(lhs);
                auto& rhs_p = call_counts.at(rhs);

                if (lhs_p.first != rhs_p.first) {
                  return lhs_p.first < rhs_p.first;
                }

                if (lhs_p.second != rhs_p.second) {
                  return lhs_p.second < rhs_p.second;
                }

                return compare_dexmethods(lhs, rhs);
              });

    SimpleOrdering::Map ret;
    for (size_t i = 0; i < profile_methods.size(); ++i) {
      // +1 to have 0 empty for methods without profile.
      ret.emplace(profile_methods[i],
                  ((double)i + 1) / (profile_methods.size() + 1));
    }

    return ret;
  }
};

struct SimpleOrderingProvider {
  mutable std::once_flag flag;
  const method_profiles::MethodProfiles& profiles;
  mutable SimpleOrdering ordering;

  explicit SimpleOrderingProvider(
      const method_profiles::MethodProfiles& profiles)
      : profiles(profiles) {}

  const SimpleOrdering& operator()() const {
    std::call_once(flag, [&]() { ordering = SimpleOrdering(profiles); });
    return ordering;
  }
};

template <typename OrderingProvider>
class MergePairsBuilder {
  using MergablesMap = std::unordered_map<const DexMethod*, const DexMethod*>;

 public:
  using PairSeq = std::vector<std::pair<const DexMethod*, const DexMethod*>>;

  MergePairsBuilder(const VirtualScope* virtual_scope,
                    const OrderingProvider& ordering_provider,
                    const VirtualMerging::PerfConfig& perf_config)
      : virtual_scope(virtual_scope),
        m_ordering_provider(ordering_provider),
        m_perf_config(perf_config) {}

  boost::optional<std::pair<LocalStats, PairSeq>> build(
      const std::unordered_set<const DexMethod*>& mergeable_methods,
      const XStoreRefs& xstores,
      const XDexRefs& xdexes,
      const method_profiles::MethodProfiles& profiles,
      VirtualMerging::Strategy strategy) {
    if (!init()) {
      return boost::none;
    }

    MergablesMap mergeable_pairs_map =
        find_overrides(mergeable_methods, xstores, xdexes);

    if (mergeable_pairs_map.empty()) {
      always_assert(stats.overriding_methods ==
                    stats.cross_store_refs + stats.cross_dex_refs +
                        stats.inconcrete_overridden_methods);
      return std::make_pair(stats, PairSeq{});
    }

    auto mergeable_pairs =
        create_merge_pair_sequence(mergeable_pairs_map, profiles, strategy);
    return std::make_pair(stats, mergeable_pairs);
  }

 private:
  bool init() {
    for (auto& p : virtual_scope->methods) {
      auto method = p.first;
      methods.push_back(method);
      types_to_methods.emplace(method->get_class(), method);
      if (!can_rename(method) || root(method) ||
          method->rstate.no_optimizations()) {
        // If we find any method in this virtual scope which we shouldn't
        // touch, we exclude the entire virtual scope.
        return false;
      }
    }
    return true;
  }

  MergablesMap find_overrides(
      const std::unordered_set<const DexMethod*>& mergeable_methods,
      const XStoreRefs& xstores,
      const XDexRefs& xdexes) {
    MergablesMap mergeable_pairs_map;
    // sorting to make things deterministic
    std::sort(methods.begin(), methods.end(), dexmethods_comparator());
    for (DexMethod* overriding_method : methods) {
      if (!mergeable_methods.count(overriding_method)) {
        continue;
      }
      stats.overriding_methods++;
      auto subtype = overriding_method->get_class();
      always_assert(subtype != virtual_scope->type);
      auto overriding_cls = type_class(overriding_method->get_class());
      always_assert(overriding_cls != nullptr);
      auto supertype = overriding_cls->get_super_class();
      always_assert(supertype != nullptr);

      auto run_fn = [](auto fn, DexType* start, DexType* trailing,
                       const DexType* stop) {
        for (;;) {
          if (fn(start, trailing)) {
            return true;
          }
          if (start == stop) {
            return false;
          }
          trailing = start;
          start = type_class(start)->get_super_class();
        }
      };

      run_fn(
          [this](const DexType* t, DexType* trailing) {
            subtypes[t].push_back(trailing);
            return false;
          },
          supertype, subtype, virtual_scope->type);

      bool found_override = run_fn(
          [this, &overriding_method, &mergeable_pairs_map, &xstores,
           &xdexes](const DexType* t, const DexType*) {
            auto it = types_to_methods.find(t);
            if (it == types_to_methods.end()) {
              return false;
            }
            auto overridden_method = it->second;
            if (!overridden_method->is_concrete() ||
                is_native(overridden_method)) {
              stats.inconcrete_overridden_methods++;
            } else if (xstores.cross_store_ref(overridden_method,
                                               overriding_method)) {
              stats.cross_store_refs++;
            } else if (xdexes.cross_dex_ref_override(overridden_method,
                                                     overriding_method) ||
                       (xdexes.num_dexes() > 1 &&
                        xdexes.is_in_primary_dex(overridden_method))) {
              stats.cross_dex_refs++;
            } else {
              always_assert(overriding_method->get_code());
              always_assert(is_abstract(overridden_method) ||
                            overridden_method->get_code());
              mergeable_pairs_map.emplace(overriding_method, overridden_method);
            }
            return true;
          },
          supertype, subtype, virtual_scope->type);
      always_assert(found_override);
    }

    return mergeable_pairs_map;
  }

  PairSeq create_merge_pair_sequence(
      const MergablesMap& mergeable_pairs_map,
      const method_profiles::MethodProfiles& profiles,
      VirtualMerging::Strategy strategy) {
    // we do a depth-first traversal of the subtype structure, adding
    // mergeable pairs as we find them; this ensures that mergeable pairs
    // can later be processed sequentially --- first inlining pairs that
    // appear in deeper portions of the type hierarchy
    PairSeq mergeable_pairs;
    std::unordered_set<const DexType*> visited;
    std::unordered_map<const DexMethod*,
                       std::vector<std::pair<const DexMethod*, double>>>
        override_map;

    size_t perf_skipped{0};
    self_recursive_fn(
        [&](auto self, const DexType* t) {
          if (visited.count(t)) {
            return;
          }
          visited.insert(t);

          auto subtypes_it = subtypes.find(t);
          if (subtypes_it != subtypes.end()) {
            // This is ordered because `methods` was ordered.
            for (auto subtype : subtypes_it->second) {
              self(self, subtype);
            }
          }

          const DexMethod* t_method;
          {
            auto t_method_it = types_to_methods.find(t);
            if (t_method_it == types_to_methods.end()) {
              return;
            }
            t_method = t_method_it->second;
          }

          double order_value = 0;
          enum OrderMix {
            kSum,
            kMax,
          };
          OrderMix order_mix = OrderMix::kSum;

          switch (strategy) {
          case VirtualMerging::Strategy::kLexicographical:
            break;
          case VirtualMerging::Strategy::kProfileCallCount:
            if (auto mstats = profiles.get_method_stat(
                    method_profiles::COLD_START, t_method)) {
              order_value = mstats->call_count;
            }
            break;
          case VirtualMerging::Strategy::kProfileAppearBucketsAndCallCount: {
            // Using appear100 with buckets, and adding in normalized
            // call-count.
            //
            // To merge interactions, give precedence to cold-start for bucket.
            // If a method is not executed during cold-start, sort it into the
            // next lower bucket.
            auto cold_stats =
                profiles.get_method_stat(method_profiles::COLD_START, t_method);
            double appear_part;
            if (cold_stats) {
              appear_part =
                  std::floor(cold_stats->appear_percent / kAppear100Buckets) *
                  kAppear100Buckets;
            } else {
              double max_appear{0};
              for (auto& i : profiles.all_interactions()) {
                auto it = i.second.find(t_method);
                if (it != i.second.end()) {
                  max_appear = std::max(max_appear, it->second.appear_percent);
                }
              }
              appear_part =
                  std::max(0.0,
                           std::floor(max_appear / kAppear100Buckets - 1) *
                               kAppear100Buckets);
            }

            double call_part = m_ordering_provider().get_order(t_method);
            order_value = appear_part + call_part;
            // Summing up does not make much sense here and would overvalue
            // multiple appear subcalls over single but high-call-count ones.
            order_mix = OrderMix::kMax;
            break;
          }
          }

          const bool should_keep = [&]() {
            if (!profiles.has_stats()) {
              return false;
            }
            auto opt_stat = profiles.get_method_stat("ColdStart", t_method);
            if (!opt_stat) {
              return false;
            }
            if (opt_stat->appear_percent < m_perf_config.appear100_threshold ||
                opt_stat->call_count < m_perf_config.call_count_threshold) {
              return false;
            }
            return true;
          }();

          if (should_keep) {
            auto it = override_map.find(t_method);
            if (it != override_map.end()) {
              auto& t_overrides = it->second;
              redex_assert(!t_overrides.empty());
              perf_skipped += t_overrides.size();

              // Clear the vector. Leave it empty for the assert above
              // (to ensure things are not handled twice).
              t_overrides.clear();
              t_overrides.shrink_to_fit();
            }
            auto overridden_method_it = mergeable_pairs_map.find(t_method);
            if (overridden_method_it != mergeable_pairs_map.end()) {
              perf_skipped++;
            }
          } else {
            {
              // If there are overrides for this type's implementation, order
              // the overrides by their weight (and otherwise retain the
              // original order), then insert the overrides into the global
              // merge structure.
              auto it = override_map.find(t_method);
              if (it != override_map.end()) {
                auto& t_overrides = it->second;
                redex_assert(!t_overrides.empty());
                // Use stable sort to retain order if other ordering is
                // unavailable. As insertion is pushing to front, sort low to
                // high.
                std::stable_sort(t_overrides.begin(), t_overrides.end(),
                                 [](const auto& lhs, const auto& rhs) {
                                   return lhs.second < rhs.second;
                                 });
                for (const auto& p : t_overrides) {
                  auto assert_it = mergeable_pairs_map.find(p.first);
                  redex_assert(assert_it != mergeable_pairs_map.end());
                  if (perf_skipped == 0) {
                    redex_assert(assert_it->second == t_method);
                  } else if (assert_it->second != t_method) {
                    // When skipped for perf, we should find the elements as
                    // "descendants."
                    auto* cur_m = assert_it->second;
                    while (cur_m != nullptr && cur_m != t_method) {
                      auto cur_it = mergeable_pairs_map.find(cur_m);
                      cur_m = (cur_it != mergeable_pairs_map.end())
                                  ? cur_it->second
                                  : nullptr;
                    }
                    redex_assert(cur_m == t_method);
                  }

                  mergeable_pairs.emplace_back(t_method, p.first);
                  switch (order_mix) {
                  case OrderMix::kSum:
                    order_value += p.second;
                    break;
                  case OrderMix::kMax:
                    order_value = std::max(order_value, p.second);
                    break;
                  }
                }
                // Clear the vector. Leave it empty for the assert above
                // (to ensure things are not handled twice).
                t_overrides.clear();
                t_overrides.shrink_to_fit();
              }
            }

            auto overridden_method_it = mergeable_pairs_map.find(t_method);
            if (overridden_method_it == mergeable_pairs_map.end()) {
              return;
            }

            override_map[overridden_method_it->second].emplace_back(
                t_method, order_value);
          }
        },
        virtual_scope->type);
    for (const auto& p : override_map) {
      redex_assert(p.second.empty());
    }
    always_assert_log(mergeable_pairs_map.size() ==
                          mergeable_pairs.size() + perf_skipped,
                      "%zu != %zu = %zu + %zu", mergeable_pairs_map.size(),
                      mergeable_pairs.size() + perf_skipped,
                      mergeable_pairs.size(), perf_skipped);
    stats.perf_skipped = perf_skipped;
    always_assert(stats.overriding_methods ==
                  mergeable_pairs.size() + stats.cross_store_refs +
                      stats.cross_dex_refs +
                      stats.inconcrete_overridden_methods + stats.perf_skipped);
    return mergeable_pairs;
  }

  const VirtualScope* virtual_scope;
  const OrderingProvider& m_ordering_provider;
  std::vector<DexMethod*> methods;
  std::unordered_map<const DexType*, DexMethod*> types_to_methods;
  std::unordered_map<const DexType*, std::vector<DexType*>> subtypes;
  LocalStats stats;
  const VirtualMerging::PerfConfig& m_perf_config;
};

} // namespace

// Part 3: For each virtual scope, identify all pairs of methods where
//         one can be merged with another. The list of pairs is ordered in
//         way that it can be later processed sequentially.
VirtualMerging::MergablePairsByVirtualScope
VirtualMerging::compute_mergeable_pairs_by_virtual_scopes(
    const method_profiles::MethodProfiles& profiles,
    Strategy strategy,
    VirtualMergingStats& stats) const {
  ConcurrentMap<const VirtualScope*, LocalStats> local_stats;
  std::vector<const VirtualScope*> virtual_scopes;
  for (auto& p : m_mergeable_scope_methods) {
    virtual_scopes.push_back(p.first);
  }
  ConcurrentMap<const VirtualScope*,
                std::vector<std::pair<const DexMethod*, const DexMethod*>>>
      mergeable_pairs_by_virtual_scopes;
  SimpleOrderingProvider ordering_provider{profiles};
  walk::parallel::virtual_scopes(
      virtual_scopes, [&](const VirtualScope* virtual_scope) {
        MergePairsBuilder mpb(virtual_scope, ordering_provider, m_perf_config);
        auto res = mpb.build(m_mergeable_scope_methods.at(virtual_scope),
                             m_xstores, m_xdexes, profiles, strategy);
        if (!res) {
          return;
        }
        local_stats.emplace(virtual_scope, res->first);
        if (!res->second.empty()) {
          mergeable_pairs_by_virtual_scopes.emplace(virtual_scope,
                                                    std::move(res->second));
        }
      });

  stats.virtual_scopes_with_mergeable_pairs +=
      mergeable_pairs_by_virtual_scopes.size();

  size_t overriding_methods = 0;
  for (auto& p : local_stats) {
    overriding_methods += p.second.overriding_methods;
    stats.cross_store_refs += p.second.cross_store_refs;
    stats.cross_dex_refs += p.second.cross_dex_refs;
    stats.inconcrete_overridden_methods +=
        p.second.inconcrete_overridden_methods;
    stats.perf_skipped += p.second.perf_skipped;
  }

  always_assert(overriding_methods <= stats.mergeable_virtual_methods);
  stats.annotated_methods =
      stats.mergeable_virtual_methods - overriding_methods;

  MergablePairsByVirtualScope out;
  for (auto& p : mergeable_pairs_by_virtual_scopes) {
    const auto& mergeable_pairs = p.second;
    stats.mergeable_pairs += mergeable_pairs.size();
    out.insert(p);
  }
  always_assert(mergeable_pairs_by_virtual_scopes.size() == out.size());
  always_assert(stats.mergeable_pairs ==
                stats.mergeable_virtual_methods - stats.annotated_methods -
                    stats.cross_store_refs - stats.cross_dex_refs -
                    stats.inconcrete_overridden_methods - stats.perf_skipped);

  return out;
}

namespace {

using MethodData = std::pair<
    const DexMethod*,
    std::vector<std::pair<const VirtualScope*, std::vector<const DexMethod*>>>>;

std::pair<std::vector<MethodData>, VirtualMergingStats> create_ordering(
    const VirtualMerging::MergablePairsByVirtualScope& mergable_pairs,
    size_t max_overriding_method_instructions,
    MultiMethodInliner& inliner) {
  std::vector<MethodData> ordering;
  VirtualMergingStats stats;

  // Fill the ordering.
  {
    std::unordered_map<const DexMethod*, size_t> method_idx;

    for (auto& p : mergable_pairs) {
      auto virtual_scope = p.first;
      const auto& mergeable_pairs = p.second;
      for (auto& q : mergeable_pairs) {
        auto overridden_method = q.first;
        auto overriding_method = q.second;

        MethodData* method_data;
        {
          auto it = method_idx.find(overridden_method);
          if (it == method_idx.end()) {
            ordering.emplace_back(
                overridden_method,
                std::vector<std::pair<const VirtualScope*,
                                      std::vector<const DexMethod*>>>{});
            method_idx.emplace(overridden_method, ordering.size() - 1);
            method_data = &ordering.back();
          } else {
            method_data = &ordering.at(it->second);
          }
        }

        if (method_data->second.empty() ||
            method_data->second.back().first != virtual_scope) {
          method_data->second.emplace_back(virtual_scope,
                                           std::vector<const DexMethod*>{});
        }
        std::vector<const DexMethod*>& v_data =
            method_data->second.back().second;
        v_data.push_back(overriding_method);
      }
    }

    for (const auto& p : ordering) {
      std::unordered_set<const VirtualScope*> scopes_seen;
      for (const auto& q : p.second) {
        redex_assert(scopes_seen.count(q.first) == 0);
        scopes_seen.insert(q.first);
      }
    }
  }

  // Sort out large methods already.
  for (auto& p : ordering) {
    auto overridden_method = const_cast<DexMethod*>(p.first);
    for (auto& q : p.second) {
      q.second.erase(
          std::remove_if(
              q.second.begin(),
              q.second.end(),
              [&](const auto* m) {
                size_t estimated_callee_size =
                    m->get_code()->sum_opcode_sizes();
                if (estimated_callee_size >
                    max_overriding_method_instructions) {
                  TRACE(VM,
                        5,
                        "[VM] %s is too large to be merged into %s",
                        SHOW(m),
                        SHOW(overridden_method));
                  stats.huge_methods++;
                  return true;
                }

                size_t estimated_caller_size =
                    is_abstract(overridden_method)
                        ? 64 // we'll need some extra instruction; 64
                             // is conservative
                        : overridden_method->get_code()->sum_opcode_sizes();
                if (!inliner.is_inlinable(
                        overridden_method, m, nullptr /* invoke_virtual_insn */,
                        estimated_caller_size, estimated_callee_size)) {
                  TRACE(VM,
                        3,
                        "[VM] Cannot inline %s into %s",
                        SHOW(m),
                        SHOW(overridden_method));
                  stats.uninlinable_methods++;
                  return true;
                }

                return false;
              }),
          q.second.end());
    }

    // Check whether it is likely that we'll be able to inline everything.
    {
      size_t sum = is_abstract(overridden_method)
                       ? 64 // we'll need some extra instruction; 64
                            // is conservative
                       : overridden_method->get_code()->sum_opcode_sizes();

      auto method_inline_estimate = [](const DexMethod* m) {
        return 20 // if + invoke + return ~= 20.
               + m->get_code()->sum_opcode_sizes();
      };

      size_t num_methods = 0;
      for (auto& q : p.second) {
        num_methods += q.second.size();
        for (auto* m : q.second) {
          sum += method_inline_estimate(m);
        }
      }

      // The inliner uses a limit of 1<<15 - 1<<12. Let's use 1<<14, which
      // is hopefully conservative.
      constexpr size_t kLimit = (1u << 15) - (1u << 13);
      if (kLimit < sum) {
        TRACE(VM,
              3,
              "[VM] Estimated sum of inlines too large for %s: %zu",
              SHOW(overridden_method),
              sum);

        // To be consistent with other orderings, we need to be
        // any-order-deterministic when removing candidates. It would probably
        // be good to do this well, e.g., work towards being able to remove
        // the most methods. But let's be simple for now.
        std::unordered_map<const VirtualScope*, std::vector<const DexMethod*>*>
            data_map;
        data_map.reserve(p.second.size());
        std::vector<const VirtualScope*> scopes;
        scopes.reserve(p.second.size());

        for (auto& q : p.second) {
          scopes.push_back(q.first);
          data_map.emplace(q.first, &q.second);
        }
        // Sort scopes by root methods. This is somewhat arbitrary but stable.
        std::sort(scopes.begin(), scopes.end(),
                  [](const auto* lhs, const auto* rhs) {
                    if (lhs == rhs) {
                      return false;
                    }
                    return compare_dexmethods(lhs->methods.front().first,
                                              rhs->methods.front().first);
                  });

        size_t removals = 0;
        for (const auto* scope : scopes) {
          auto m_tmp = *data_map.at(scope);
          // Sort methods lexicographically. Arbitrary but stable. Could include
          // size.
          std::sort(m_tmp.begin(), m_tmp.end(), compare_dexmethods);

          // Fetch methods to get under limit.
          std::unordered_set<const DexMethod*> to_remove;
          for (auto* m : m_tmp) {
            sum -= method_inline_estimate(m);
            to_remove.insert(m);
            if (sum <= kLimit) {
              break;
            }
          }

          // Remove those methods.
          auto* m_orig = data_map.at(scope);
          m_orig->erase(std::remove_if(m_orig->begin(), m_orig->end(),
                                       [&to_remove](const auto* m) {
                                         return to_remove.count(m) != 0;
                                       }),
                        m_orig->end());
          removals += to_remove.size();

          if (sum <= kLimit) {
            break;
          }
        }
        TRACE(VM,
              3,
              "[VM] Removed %zu of %zu methods to reduce estimate for %s",
              removals,
              num_methods,
              SHOW(overridden_method));
        stats.caller_size_removed_methods += removals;
      }
    }
  }

  // Remove methods that no longer have inlinees.
  ordering.erase(std::remove_if(ordering.begin(), ordering.end(),
                                [](const auto& p) {
                                  size_t sum = 0;
                                  for (const auto& q : p.second) {
                                    sum += q.second.size();
                                  }
                                  return sum == 0;
                                }),
                 ordering.end());

  return std::make_pair(std::move(ordering), stats);
}

struct SBHelper {
  DexMethod* overridden;
  const std::vector<const DexMethod*>& v;
  const bool overridden_had_source_blocks;
  const bool create_source_blocks;

  explicit SBHelper(DexMethod* overridden,
                    const std::vector<const DexMethod*>& v)
      : overridden(overridden),
        v(v),
        overridden_had_source_blocks(
            overridden->get_code() != nullptr &&
            source_blocks::get_first_source_block_of_method(overridden) !=
                nullptr),
        create_source_blocks([&]() {
          for (auto* m : v) {
            if (source_blocks::get_first_source_block_of_method(m) != nullptr) {
              return true;
            }
          }
          return false;
        }()) {
    // Fix up the host with empty source blocks if necessary. It's easier to
    // do this ahead of time.
    if (create_source_blocks && !overridden_had_source_blocks &&
        overridden->get_code() != nullptr) {
      source_blocks::insert_synthetic_source_blocks_in_method(
          overridden, get_source_block_creator());
    }
  }

  SourceBlock* get_arbitrary_first_sb() const {
    auto sb = source_blocks::get_any_first_source_block_of_methods(v);
    always_assert(sb != nullptr);
    return sb;
  }

  std::function<std::unique_ptr<SourceBlock>()> get_source_block_creator(
      float val = 0) const {
    return [overridden = this->overridden,
            template_sb = get_arbitrary_first_sb(), val]() {
      auto new_sb = std::make_unique<SourceBlock>(*template_sb);
      source_blocks::fill_source_block(*new_sb, overridden,
                                       SourceBlock::kSyntheticId,
                                       SourceBlock::Val{val, 0});
      return new_sb;
    };
  }

  struct ScopedSplitHelper {
    cfg::Block* block{nullptr};
    SourceBlock* first_sb{nullptr};
    DexMethod* overriding{nullptr};
    SBHelper* parent{nullptr};

    ScopedSplitHelper(cfg::Block* block,
                      IRList::iterator last_it,
                      DexMethod* overriding,
                      SBHelper* parent)
        : block(block),
          first_sb([&]() -> SourceBlock* {
            for (auto it = std::next(last_it); it != block->end(); ++it) {
              if (it->type == MFLOW_SOURCE_BLOCK) {
                return it->src_block.get();
              }
            }
            return nullptr;
          }()),
          overriding(overriding),
          parent(parent) {}

    ~ScopedSplitHelper() {
      if (block != nullptr) {
        auto overriding_sb =
            source_blocks::get_first_source_block_of_method(overriding);
        auto new_sb = std::make_unique<SourceBlock>(
            overriding_sb != nullptr ? *overriding_sb
            : first_sb != nullptr    ? *first_sb
                                     : *parent->get_arbitrary_first_sb());
        new_sb->src = parent->overridden->get_deobfuscated_name_or_null();
        new_sb->id = SourceBlock::kSyntheticId;
        if (overriding_sb != nullptr && first_sb != nullptr) {
          for (size_t i = 0; i != new_sb->vals_size; ++i) {
            if (!new_sb->get_val(i)) {
              new_sb->vals[i] = first_sb->vals[i];
            } else if (first_sb->get_val(i)) {
              new_sb->vals[i]->val += first_sb->vals[i]->val;
              new_sb->vals[i]->appear100 =
                  std::max(new_sb->vals[i]->appear100, first_sb->vals[i]->val);
            }
          }
        }
        block->insert_before(block->end(), std::move(new_sb));
      }
    }

    ScopedSplitHelper(const ScopedSplitHelper&) = delete;
    ScopedSplitHelper(ScopedSplitHelper&& rhs) noexcept
        : block(rhs.block),
          first_sb(rhs.first_sb),
          overriding(rhs.overriding),
          parent(rhs.parent) {
      rhs.block = nullptr;
    }

    ScopedSplitHelper& operator=(const ScopedSplitHelper&) = delete;
    ScopedSplitHelper& operator=(ScopedSplitHelper&& rhs) noexcept {
      block = rhs.block;
      first_sb = rhs.first_sb;
      overriding = rhs.overriding;
      parent = rhs.parent;

      rhs.block = nullptr;

      return *this;
    }
  };

  std::optional<ScopedSplitHelper> handle_split(cfg::Block* block,
                                                const IRList::iterator& it,
                                                DexMethod* overriding) {
    if (!create_source_blocks) {
      return std::nullopt;
    }
    return ScopedSplitHelper(block, it, overriding, this);
  }

  void add_return_sb(
      DexMethod* overriding,
      const std::function<void(std::unique_ptr<SourceBlock>)>& push_sb) {
    if (create_source_blocks) {
      // Let's assume there's always normal return.
      auto o_sb = source_blocks::get_first_source_block(overriding->get_code());
      if (o_sb != nullptr) {
        auto new_sb = std::make_unique<SourceBlock>(*o_sb);
        new_sb->src = overriding->get_deobfuscated_name_or_null();
        new_sb->id = SourceBlock::kSyntheticId;
        push_sb(std::move(new_sb));
      }
    }
  }
};

VirtualMergingStats apply_ordering(
    MultiMethodInliner& inliner,
    std::vector<MethodData>& ordering,
    std::unordered_map<DexClass*, std::vector<const DexMethod*>>&
        virtual_methods_to_remove,
    std::unordered_map<DexMethod*, DexMethod*>& virtual_methods_to_remap,
    VirtualMerging::InsertionStrategy insertion_strategy) {
  VirtualMergingStats stats;
  for (auto& p : ordering) {
    auto overridden_method = const_cast<DexMethod*>(p.first);
    for (auto& q : p.second) {
      if (q.second.empty()) {
        continue;
      }
      SBHelper sb_helper(overridden_method, q.second);

      auto* virtual_scope = q.first;

      for (auto* overriding_method_const : q.second) {
        auto overriding_method =
            const_cast<DexMethod*>(overriding_method_const);
        size_t estimated_callee_size =
            overriding_method->get_code()->sum_opcode_sizes();
        size_t estimated_insn_size =
            is_abstract(overridden_method)
                ? 64 // we'll need some extra instruction; 64 is conservative
                : overridden_method->get_code()->sum_opcode_sizes();
        bool is_inlineable =
            inliner.is_inlinable(overridden_method, overriding_method,
                                 nullptr /* invoke_virtual_insn */,
                                 estimated_insn_size, estimated_callee_size);
        always_assert_log(is_inlineable, "[VM] Cannot inline %s into %s",
                          SHOW(overriding_method), SHOW(overridden_method));

        TRACE(VM,
              4,
              "[VM] Merging %s into %s",
              SHOW(overriding_method),
              SHOW(overridden_method));

        auto proto = overriding_method->get_proto();
        always_assert(overridden_method->get_proto() == proto);
        std::vector<uint32_t> param_regs;
        std::function<void(IRInstruction*)> push_insn;
        std::function<void(std::unique_ptr<SourceBlock>)> push_sb;
        std::function<uint32_t()> allocate_temp;
        std::function<uint32_t()> allocate_wide_temp;
        std::function<void()> cleanup;
        IRCode* overridden_code;
        // We make the method public to avoid visibility issues. We could be
        // more conservative (i.e. taking the strongest visibility control
        // that encompasses the original pair) but I'm not sure it's worth the
        // effort.
        set_public(overridden_method);
        if (is_abstract(overridden_method)) {
          // We'll make the abstract method be not abstract, and give it a new
          // method body.
          // It starts out with just load-param instructions as needed, and
          // then we'll add an invoke-virtual instruction that will get
          // inlined.
          stats.unabstracted_methods++;
          overridden_method->make_concrete(
              (DexAccessFlags)(overridden_method->get_access() & ~ACC_ABSTRACT),
              std::make_unique<IRCode>(),
              true /* is_virtual */);
          overridden_code = overridden_method->get_code();
          auto load_param_insn = new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
          load_param_insn->set_dest(overridden_code->allocate_temp());
          overridden_code->push_back(load_param_insn);
          param_regs.push_back(load_param_insn->dest());
          for (auto t : *proto->get_args()) {
            if (type::is_wide_type(t)) {
              load_param_insn = new IRInstruction(IOPCODE_LOAD_PARAM_WIDE);
              load_param_insn->set_dest(overridden_code->allocate_wide_temp());
            } else {
              load_param_insn = new IRInstruction(
                  type::is_object(t) ? IOPCODE_LOAD_PARAM_OBJECT
                                     : IOPCODE_LOAD_PARAM);
              load_param_insn->set_dest(overridden_code->allocate_temp());
            }
            overridden_code->push_back(load_param_insn);
            param_regs.push_back(load_param_insn->dest());
          }

          if (sb_helper.create_source_blocks) {
            overridden_code->push_back(sb_helper.get_source_block_creator()());
          }

          // we'll define helper functions in a way that lets them mutate the
          // new IRCode
          push_insn = [=](IRInstruction* insn) {
            overridden_code->push_back(insn);
          };
          push_sb = [=](std::unique_ptr<SourceBlock> sb) {
            overridden_code->push_back(std::move(sb));
          };
          allocate_temp = [=]() { return overridden_code->allocate_temp(); };
          allocate_wide_temp = [=]() {
            return overridden_code->allocate_wide_temp();
          };
          cleanup = [=]() { overridden_code->build_cfg(/* editable */ true); };
        } else {
          // We are dealing with a non-abstract method. In this case, we'll
          // first insert an if-instruction to decide whether to run the
          // overriding method that we'll inline, or whether to jump to the
          // old method body.
          overridden_code = overridden_method->get_code();
          always_assert(overridden_code);
          overridden_code->build_cfg(/* editable */ true);
          auto& overridden_cfg = overridden_code->cfg();

          // Find block with load-param instructions
          cfg::Block* block = overridden_cfg.entry_block();
          while (block->get_first_insn() == block->end()) {
            const auto& succs = block->succs();
            always_assert(succs.size() == 1);
            const auto& out = succs[0];
            always_assert(out->type() == cfg::EDGE_GOTO);
            block = out->target();
          }

          // Scan load-param instructions
          std::unordered_set<uint32_t> param_regs_set;
          auto last_it = block->end();
          for (auto it = block->begin(); it != block->end(); it++) {
            auto& mie = *it;
            if (mie.type != MFLOW_OPCODE) {
              continue;
            }
            if (!opcode::is_a_load_param(mie.insn->opcode())) {
              break;
            }
            param_regs.push_back(mie.insn->dest());
            param_regs_set.insert(mie.insn->dest());
            last_it = it;
          }
          always_assert(param_regs.size() == param_regs_set.size());
          always_assert(1 + proto->get_args()->size() == param_regs_set.size());
          always_assert(last_it != block->end());

          // We'll split the block right after the last load-param instruction
          // --- that's where we'll insert the new if-statement.
          {
            auto sb_scoped =
                sb_helper.handle_split(block, last_it, overriding_method);
            overridden_cfg.split_block(block, last_it);
          }

          auto new_block = overridden_cfg.create_block();
          {
            // instance-of param0, DeclaringTypeOfOverridingMethod
            auto instance_of_insn = new IRInstruction(OPCODE_INSTANCE_OF);
            instance_of_insn->set_type(overriding_method->get_class());
            instance_of_insn->set_src(0, param_regs.at(0));
            block->push_back(instance_of_insn);
            // move-result-pseudo if_temp
            auto if_temp_reg = overridden_cfg.allocate_temp();
            auto move_result_pseudo_insn =
                new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO);
            move_result_pseudo_insn->set_dest(if_temp_reg);
            block->push_back(move_result_pseudo_insn);

            switch (insertion_strategy) {
            case VirtualMerging::InsertionStrategy::kJumpTo: {
              // if-nez if_temp, new_code
              // (fall through to old code)
              auto if_insn = new IRInstruction(OPCODE_IF_NEZ);
              if_insn->set_src(0, if_temp_reg);
              overridden_cfg.create_branch(
                  block, if_insn, /*fls=*/block->goes_to(), /*tru=*/new_block);
              break;
            }

            case VirtualMerging::InsertionStrategy::kFallthrough: {
              // if-eqz if_temp, old code
              // (fall through to new_code)
              auto if_insn = new IRInstruction(OPCODE_IF_EQZ);
              if_insn->set_src(0, if_temp_reg);
              overridden_cfg.create_branch(block, if_insn, /*fls=*/new_block,
                                           /*tru=*/block->goes_to());
              break;
            }
            }
          }
          // we'll define helper functions in a way that lets them mutate the
          // cfg
          push_insn = [=](IRInstruction* insn) { new_block->push_back(insn); };
          auto* cfg_ptr = &overridden_cfg;
          push_sb = [=](std::unique_ptr<SourceBlock> sb) {
            new_block->insert_before(new_block->end(), std::move(sb));
          };
          allocate_temp = [=]() { return cfg_ptr->allocate_temp(); };
          allocate_wide_temp = [=]() { return cfg_ptr->allocate_wide_temp(); };
          cleanup = []() {};
        }

        if (sb_helper.create_source_blocks) {
          // Insert source block with val == 1.0 so that inlining normalizes
          // source-blocks properly
          push_sb(sb_helper.get_source_block_creator(/* val */ 1.0)());
        }

        always_assert(1 + proto->get_args()->size() == param_regs.size());

        // invoke-virtual temp, param1, ..., paramN, OverridingMethod
        auto invoke_virtual_insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
        invoke_virtual_insn->set_method(overriding_method);
        invoke_virtual_insn->set_srcs_size(param_regs.size());
        for (size_t i = 0; i < param_regs.size(); i++) {
          uint32_t reg = param_regs[i];
          if (i == 0) {
            uint32_t temp_reg = allocate_temp();
            auto check_cast_insn = new IRInstruction(OPCODE_CHECK_CAST);
            check_cast_insn->set_type(overriding_method->get_class());
            check_cast_insn->set_src(0, reg);
            push_insn(check_cast_insn);
            auto move_result_pseudo_insn =
                new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
            move_result_pseudo_insn->set_dest(temp_reg);
            push_insn(move_result_pseudo_insn);
            reg = temp_reg;
          }
          invoke_virtual_insn->set_src(i, reg);
        }
        push_insn(invoke_virtual_insn);
        if (proto->is_void()) {
          // return-void
          sb_helper.add_return_sb(overriding_method, push_sb);
          auto return_insn = new IRInstruction(OPCODE_RETURN_VOID);
          push_insn(return_insn);
        } else {
          // move-result result_temp
          auto rtype = proto->get_rtype();
          auto op = opcode::move_result_for_invoke(overriding_method);
          auto move_result_insn = new IRInstruction(op);
          auto result_temp = op == OPCODE_MOVE_RESULT_WIDE
                                 ? allocate_wide_temp()
                                 : allocate_temp();
          move_result_insn->set_dest(result_temp);
          push_insn(move_result_insn);
          sb_helper.add_return_sb(overriding_method, push_sb);
          // return result_temp
          op = opcode::return_opcode(rtype);
          auto return_insn = new IRInstruction(op);
          return_insn->set_src(0, result_temp);
          push_insn(return_insn);
        }

        cleanup();

        overriding_method->get_code()->build_cfg(/* editable */ true);
        inliner::inline_with_cfg(
            overridden_method, overriding_method, invoke_virtual_insn,
            /* needs_receiver_cast */ nullptr, /* needs_init_class */ nullptr,
            overridden_method->get_code()->cfg().get_registers_size());
        inliner.visibility_changes_apply_and_record_make_static(
            get_visibility_changes(overriding_method,
                                   overridden_method->get_class()));
        overriding_method->get_code()->clear_cfg();

        // Check if everything was inlined.
        for (const auto& mie :
             cfg::InstructionIterable(overridden_code->cfg())) {
          redex_assert(invoke_virtual_insn != mie.insn);
        }

        overridden_code->clear_cfg();

        virtual_methods_to_remove[type_class(overriding_method->get_class())]
            .push_back(overriding_method);
        auto virtual_scope_root = virtual_scope->methods.front();
        always_assert(overriding_method != virtual_scope_root.first);
        virtual_methods_to_remap.emplace(overriding_method,
                                         virtual_scope_root.first);

        stats.removed_virtual_methods++;
      }
    }
  }
  return stats;
}

} // namespace

// Part 4: For each virtual scope, merge all pairs in order, unless inlining
//         is for some reason not possible, e.g. because of code size
//         constraints. Record set of methods in each class which can be
//         removed.
void VirtualMerging::merge_methods(
    const MergablePairsByVirtualScope& mergeable_pairs,
    InsertionStrategy insertion_strategy) {
  auto ordering_pair = create_ordering(
      mergeable_pairs, m_max_overriding_method_instructions, *m_inliner);
  m_stats += ordering_pair.second;

  auto stats = apply_ordering(*m_inliner, ordering_pair.first,
                              m_virtual_methods_to_remove,
                              m_virtual_methods_to_remap, insertion_strategy);
  m_stats += stats;

  always_assert(m_stats.mergeable_pairs ==
                m_stats.huge_methods + m_stats.uninlinable_methods +
                    m_stats.caller_size_removed_methods +
                    m_stats.removed_virtual_methods);
}

// Part 5: Remove methods within classes.
void VirtualMerging::remove_methods() {
  std::vector<DexClass*> classes_with_virtual_methods_to_remove;
  for (auto& p : m_virtual_methods_to_remove) {
    classes_with_virtual_methods_to_remove.push_back(p.first);
  }

  walk::parallel::classes(
      classes_with_virtual_methods_to_remove, [&](DexClass* cls) {
        for (auto method : m_virtual_methods_to_remove.at(cls)) {
          cls->remove_method(method);
        }
      });
}

// Part 6: Remap all invoke-virtual instructions where the associated method got
// removed
void VirtualMerging::remap_invoke_virtuals() {
  walk::parallel::opcodes(
      m_scope,
      [](const DexMethod*) { return true; },
      [&](const DexMethod*, IRInstruction* insn) {
        if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
          auto method_ref = insn->get_method();
          auto method = resolve_method(method_ref, MethodSearch::Virtual);
          auto it = m_virtual_methods_to_remap.find(method);
          if (it != m_virtual_methods_to_remap.end()) {
            insn->set_method(it->second);
          }
        }
      });
}

void VirtualMerging::run(const method_profiles::MethodProfiles& profiles,
                         Strategy strategy,
                         InsertionStrategy insertion_strategy) {
  TRACE(VM, 1, "[VM] Finding unsupported virtual scopes");
  find_unsupported_virtual_scopes();
  TRACE(VM, 1, "[VM] Computing mergeable scope methods");
  compute_mergeable_scope_methods();
  TRACE(VM, 1, "[VM] Computing mergeable pairs by virtual scopes");
  auto scopes =
      compute_mergeable_pairs_by_virtual_scopes(profiles, strategy, m_stats);

  TRACE(VM, 1, "[VM] Merging methods");
  merge_methods(scopes, insertion_strategy);
  TRACE(VM, 1, "[VM] Removing methods");
  remove_methods();
  TRACE(VM, 1, "[VM] Remapping invoke-virtual instructions");
  remap_invoke_virtuals();
  TRACE(VM, 1, "[VM] Done");
}

void VirtualMergingPass::bind_config() {
  // Merging huge overriding methods into an overridden method tends to not
  // be a good idea, as it may pull in many other dependencies, and all just
  // for some small saving in number of method refs. So we impose a configurable
  // limit.
  int64_t default_max_overriding_method_instructions = 1000;
  bind("max_overriding_method_instructions",
       default_max_overriding_method_instructions,
       m_max_overriding_method_instructions);
  std::string strategy;
  bind("strategy", "call-count", strategy);
  std::string insertion_strategy;
  bind("insertion_strategy", "jump-to", insertion_strategy);

  bind("perf_appear100_threshold", m_perf_config.appear100_threshold,
       m_perf_config.appear100_threshold);
  bind("perf_call_count_threshold", m_perf_config.call_count_threshold,
       m_perf_config.call_count_threshold);

  after_configuration([this, strategy, insertion_strategy] {
    always_assert(m_max_overriding_method_instructions >= 0);

    auto parse_strategy = [](const std::string& s) {
      if (s == "call-count") {
        return VirtualMerging::Strategy::kProfileCallCount;
      }
      if (s == "lexicographical") {
        return VirtualMerging::Strategy::kLexicographical;
      }
      if (s == "appear-buckets") {
        return VirtualMerging::Strategy::kProfileAppearBucketsAndCallCount;
      }
      always_assert_log(false, "Unknown strategy %s", s.c_str());
    };

    m_strategy = parse_strategy(strategy);

    auto parse_insertion_strategy = [](const std::string& s) {
      if (s == "jump-to") {
        return VirtualMerging::InsertionStrategy::kJumpTo;
      }
      if (s == "fallthrough") {
        return VirtualMerging::InsertionStrategy::kFallthrough;
      }
      always_assert_log(false, "Unknown insertion strategy %s", s.c_str());
    };

    m_insertion_strategy = parse_insertion_strategy(insertion_strategy);
  });
}

void VirtualMergingPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  if (mgr.get_redex_options().instrument_pass_enabled) {
    TRACE(VM,
          1,
          "Skipping VirtualMergingPass because Instrumentation is enabled");
    return;
  }

  auto dedupped = dedup_vmethods::dedup(stores);

  const api::AndroidSDK* min_sdk_api{nullptr};
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  mgr.incr_metric("min_sdk", min_sdk);
  TRACE(INLINE, 2, "min_sdk: %d", min_sdk);
  auto min_sdk_api_file = conf.get_android_sdk_api_file(min_sdk);
  if (!min_sdk_api_file) {
    mgr.incr_metric("min_sdk_no_file", 1);
    TRACE(INLINE, 2, "Android SDK API %d file cannot be found.", min_sdk);
  } else {
    min_sdk_api = &conf.get_android_sdk_api(min_sdk);
  }

  auto inliner_config = conf.get_inliner_config();
  // We don't need to worry about inlining synchronized code, as we always
  // inline at the top-level outside of other try-catch regions.
  inliner_config.respect_sketchy_methods = false;
  VirtualMerging vm(stores, inliner_config,
                    m_max_overriding_method_instructions, min_sdk_api,
                    m_perf_config);
  vm.run(conf.get_method_profiles(), m_strategy, m_insertion_strategy);
  auto stats = vm.get_stats();

  mgr.incr_metric(METRIC_DEDUPPED_VIRTUAL_METHODS, dedupped);
  mgr.incr_metric(METRIC_INVOKE_SUPER_METHODS, stats.invoke_super_methods);
  mgr.incr_metric(METRIC_INVOKE_SUPER_UNRESOLVED_METHOD_REFS,
                  stats.invoke_super_unresolved_method_refs);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS,
                  stats.mergeable_virtual_methods);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS_ANNOTATED_METHODS,
                  stats.annotated_methods);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_STORE_REFS,
                  stats.cross_store_refs);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_DEX_REFS,
                  stats.cross_dex_refs);
  mgr.incr_metric(
      METRIC_MERGEABLE_VIRTUAL_METHODS_INCONCRETE_OVERRIDDEN_METHODS,
      stats.inconcrete_overridden_methods);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_SCOPES,
                  stats.mergeable_scope_methods);
  mgr.incr_metric(METRIC_MERGEABLE_PAIRS, stats.mergeable_pairs);
  mgr.incr_metric(METRIC_VIRTUAL_SCOPES_WITH_MERGEABLE_PAIRS,
                  stats.virtual_scopes_with_mergeable_pairs);
  mgr.incr_metric(METRIC_UNABSTRACTED_METHODS, stats.unabstracted_methods);
  mgr.incr_metric(METRIC_UNINLINABLE_METHODS, stats.uninlinable_methods);
  mgr.incr_metric(METRIC_HUGE_METHODS, stats.huge_methods);
  mgr.incr_metric(METRIC_CALLER_SIZE_REMOVED_METHODS,
                  stats.caller_size_removed_methods);
  mgr.incr_metric(METRIC_REMOVED_VIRTUAL_METHODS,
                  stats.removed_virtual_methods);

  mgr.incr_metric("num_mergeable.perf_skipped", stats.perf_skipped);
}

static VirtualMergingPass s_pass;
