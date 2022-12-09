/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DedupStrings.h"

#include <vector>

#include "CFGMutation.h"
#include "ConcurrentContainers.h"
#include "Creators.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InstructionSequenceOutliner.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"
#include "locator.h"

namespace {

constexpr const char* DEDUP_STRINGS_CLASS_NAME_PREFIX = "Lcom/redex/Strings$";

constexpr const char* METRIC_PERF_SENSITIVE_STRINGS =
    "num_perf_sensitive_strings";
constexpr const char* METRIC_NON_PERF_SENSITIVE_STRINGS =
    "num_non_perf_sensitive_strings";
constexpr const char* METRIC_PERF_SENSITIVE_METHODS =
    "num_perf_sensitive_methods";
constexpr const char* METRIC_NON_PERF_SENSITIVE_METHODS =
    "num_non_perf_sensitive_methods";
constexpr const char* METRIC_PERF_SENSITIVE_INSNS = "num_perf_sensitive_insns";
constexpr const char* METRIC_NON_PERF_SENSITIVE_INSNS =
    "num_non_perf_sensitive_insns";
constexpr const char* METRIC_DUPLICATE_STRINGS = "num_duplicate_strings";
constexpr const char* METRIC_DUPLICATE_STRINGS_SIZE = "duplicate_strings_size";
constexpr const char* METRIC_DUPLICATE_STRING_LOADS =
    "num_duplicate_string_loads";
constexpr const char* METRIC_EXPECTED_SIZE_REDUCTION =
    "expected_size_reduction";
constexpr const char* METRIC_DEXES_WITHOUT_HOST = "num_dexes_without_host";
constexpr const char* METRIC_EXCLUDED_DUPLICATE_NON_LOAD_STRINGS =
    "num_excluded_duplicate_non_load_strings";
constexpr const char* METRIC_FACTORY_METHODS = "num_factory_methods";
constexpr const char* METRIC_EXCLUDED_OUT_OF_FACTORY_METHODS_STRINGS =
    "num_excluded_out_of_factory_methods_strings";
} // namespace

void DedupStrings::run(DexStoresVector& stores) {
  // For now, we are only trying to optimize strings in the first store.
  // (It should be possible to generalize in the future.)
  DexClassesVector& dexen = stores[0].get_dexen();
  const auto scope = build_class_scope(dexen);

  // For each method, remember which dex it's defined in.
  std::unordered_map<const DexMethod*, size_t> methods_to_dex =
      get_methods_to_dex(dexen);

  // Gather set of methods that must not be touched because they are
  // in the primary dex or perf sensitive
  std::unordered_set<const DexMethod*> perf_sensitive_methods =
      get_perf_sensitive_methods(dexen);

  // Compute set of non-load strings in each dex
  std::unordered_set<const DexString*> non_load_strings[dexen.size()];
  workqueue_run_for<size_t>(0, dexen.size(), [&](size_t i) {
    auto& strings = non_load_strings[i];
    gather_non_load_strings(dexen[i], &strings);
  });

  // For each string, figure out how many times it's loaded per dex
  ConcurrentMap<const DexString*, std::unordered_map<size_t, size_t>>
      occurrences = get_occurrences(scope, methods_to_dex,
                                    perf_sensitive_methods, non_load_strings);

  // Use heuristics to determine which strings to dedup,
  // and figure out factory method details
  std::unordered_map<const DexString*, DedupStrings::DedupStringInfo>
      strings_to_dedup =
          get_strings_to_dedup(dexen, occurrences, methods_to_dex,
                               perf_sensitive_methods, non_load_strings);

  // Rewrite const-string instructions
  rewrite_const_string_instructions(scope, methods_to_dex,
                                    perf_sensitive_methods, strings_to_dedup);
}

std::unordered_set<const DexMethod*> DedupStrings::get_perf_sensitive_methods(
    const DexClassesVector& dexen) {
  std::unordered_set<const DexMethodRef*> sufficiently_popular_methods;
  if (m_method_profiles.has_stats()) {
    for (auto& p : m_method_profiles.all_interactions()) {
      auto& method_stats = p.second;
      for (auto& q : method_stats) {
        if (q.second.appear_percent >=
            m_method_profiles_appear_percent_threshold) {
          sufficiently_popular_methods.insert(q.first);
        }
      }
    }
  }
  auto is_perf_sensitive = [&](size_t dexnr, DexClass* cls,
                               DexMethod* method) -> bool {
    // All methods in the primary dex 0 must not be touched.
    // If method-profiles are available, we treat all popular methods as
    // perf-sensitive. Otherwise, we treat all methods of perf sensitive classes
    // as perf-sensitive. We also choose to not dedup strings in cl_inits and
    // outlined methods, as they either tend to get called during critical
    // initialization code paths, or often.
    if (dexnr == 0 || method::is_clinit(method) ||
        type_class(method->get_class())->rstate.outlined()) {
      return true;
    }
    if (m_legacy_perf_logic) {
      // We used to have some strange logic for perf-sensitivity. Avoid using
      // it.
      if (!cls->is_perf_sensitive()) {
        return false;
      }
      return !m_method_profiles.has_stats() ||
             !sufficiently_popular_methods.count(method);
    }
    if (!m_method_profiles.has_stats()) {
      return cls->is_perf_sensitive();
    }
    return sufficiently_popular_methods.count(method);
  };
  std::unordered_set<const DexMethod*> perf_sensitive_methods;
  for (size_t dexnr = 0; dexnr < dexen.size(); dexnr++) {
    auto& classes = dexen[dexnr];
    for (auto cls : classes) {
      auto process_method = [&](DexMethod* method) {
        if (method->get_code() != nullptr) {
          if (is_perf_sensitive(dexnr, cls, method)) {
            perf_sensitive_methods.emplace(method);
            m_stats.perf_sensitive_methods++;
          } else {
            m_stats.non_perf_sensitive_methods++;
          }
        }
      };

      auto& dmethods = cls->get_dmethods();
      std::for_each(dmethods.begin(), dmethods.end(), process_method);
      auto& vmethods = cls->get_vmethods();
      std::for_each(vmethods.begin(), vmethods.end(), process_method);
    }
  }
  return perf_sensitive_methods;
}

std::unordered_map<const DexMethod*, size_t> DedupStrings::get_methods_to_dex(
    const DexClassesVector& dexen) {
  // Let's build a mapping that tells us for each method which dex it is
  // defined in.

  std::unordered_map<const DexMethod*, size_t> methods_to_dex;
  for (size_t dexnr = 0; dexnr < dexen.size(); dexnr++) {
    auto& classes = dexen[dexnr];
    for (auto cls : classes) {
      for (auto method : cls->get_dmethods()) {
        if (method->get_code() != nullptr) {
          methods_to_dex.emplace(method, dexnr);
        }
      }
      for (auto method : cls->get_vmethods()) {
        if (method->get_code() != nullptr) {
          methods_to_dex.emplace(method, dexnr);
        }
      }
    }
  }
  return methods_to_dex;
}

DexMethod* DedupStrings::make_const_string_loader_method(
    DexClasses& dex,
    size_t dex_id,
    const std::vector<const DexString*>& strings) {
  // Create a new class to host the string lookup method
  auto host_cls_name =
      DexString::make_string(std::string(DEDUP_STRINGS_CLASS_NAME_PREFIX) +
                             std::to_string(dex_id) + ";");
  auto host_type = DexType::make_type(host_cls_name);
  ClassCreator host_cls_creator(host_type);
  host_cls_creator.set_access(ACC_PUBLIC | ACC_FINAL);
  host_cls_creator.set_super(type::java_lang_Object());
  auto host_cls = host_cls_creator.create();
  host_cls->rstate.set_generated();
  host_cls->set_perf_sensitive(true);
  // Insert class at beginning of dex, but after canary class, if any
  auto dex_it = dex.begin();
  for (; dex_it != dex.end() && interdex::is_canary(*dex_it); dex_it++) {
  }
  dex.insert(dex_it, host_cls);

  // Here we build the string lookup method with a big switch statement.
  always_assert(!strings.empty());
  const auto string_type = type::java_lang_String();
  const auto proto = DexProto::make_proto(
      string_type, DexTypeList::make_type_list({type::_int()}));
  MethodCreator method_creator(host_cls->get_type(),
                               DexString::make_string("lookup"),
                               proto,
                               ACC_PUBLIC | ACC_STATIC);
  redex_assert(!strings.empty());
  auto id_arg = method_creator.get_local(0);
  auto res_var = method_creator.make_local(type::java_lang_String());
  auto main_block = method_creator.get_main_block();

  if (strings.size() == 1) {
    main_block->load_const(res_var, strings.at(0));
    main_block->ret(string_type, res_var);
  } else {
    std::map<int, MethodBlock*> cases;
    for (size_t idx = 0; idx < strings.size() - 1; ++idx) {
      cases[idx] = nullptr;
    }
    MethodBlock* default_block = main_block->switch_op(id_arg, cases);
    default_block->load_const(res_var, strings.at(strings.size() - 1));
    main_block->ret(string_type, res_var);

    for (size_t idx = 0; idx < strings.size() - 1; ++idx) {
      auto case_block = cases.at(idx);
      case_block->load_const(res_var, strings.at(idx));
      // Note that a goto instruction at the end of the case block is
      // automatically generated (and then later replaced by a return
      // instruction by the replace-gotos-with-returns pass).
    }
  }
  auto method = method_creator.create();
  host_cls->add_method(method);
  method->get_code()->build_cfg(/* editable */ true);
  return method;
}

void DedupStrings::gather_non_load_strings(
    DexClasses& classes, std::unordered_set<const DexString*>* strings) {
  // Let's figure out the set of "non-load" strings, i.e. the strings which
  // are referenced by some metadata (and not just const-string instructions)
  std::vector<const DexString*> lstring;
  std::vector<DexType*> ltype;
  std::vector<DexFieldRef*> lfield;
  std::vector<DexMethodRef*> lmethod;
  std::vector<DexCallSite*> lcallsite;
  std::vector<DexMethodHandle*> lmethodhandle;
  gather_components(lstring, ltype, lfield, lmethod, lcallsite, lmethodhandle,
                    classes,
                    /* exclude_loads */ true);

  strings->insert(lstring.begin(), lstring.end());
}

ConcurrentMap<const DexString*, std::unordered_map<size_t, size_t>>
DedupStrings::get_occurrences(
    const Scope& scope,
    const std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
    const std::unordered_set<const DexMethod*>& perf_sensitive_methods,
    std::unordered_set<const DexString*> non_load_strings[]) {
  // For each string, figure out how many times it's loaded per dex
  ConcurrentMap<const DexString*, std::unordered_map<size_t, size_t>>
      occurrences;
  ConcurrentMap<const DexString*, std::unordered_set<size_t>>
      perf_sensitive_strings;
  std::atomic<size_t> perf_sensitive_insns{0};
  std::atomic<size_t> non_perf_sensitive_insns{0};
  walk::parallel::code(
      scope, [&occurrences, &perf_sensitive_strings, &methods_to_dex,
              &perf_sensitive_methods, &perf_sensitive_insns,
              &non_perf_sensitive_insns](DexMethod* method, IRCode& code) {
        const auto dexnr = methods_to_dex.at(method);
        const auto perf_sensitive = perf_sensitive_methods.count(method) != 0;
        always_assert(code.editable_cfg_built());
        auto& cfg = code.cfg();
        size_t local_perf_sensitive_insns{0};
        size_t local_non_perf_sensitive_insns{0};
        for (auto& mie : InstructionIterable(cfg)) {
          const auto insn = mie.insn;
          if (insn->opcode() == OPCODE_CONST_STRING) {
            const auto str = insn->get_string();
            if (perf_sensitive) {
              perf_sensitive_strings.update(
                  str,
                  [dexnr](const DexString*,
                          std::unordered_set<size_t>& s,
                          bool /* exists */) { s.emplace(dexnr); });
              local_perf_sensitive_insns++;
            } else {
              occurrences.update(str,
                                 [dexnr](const DexString*,
                                         std::unordered_map<size_t, size_t>& m,
                                         bool /* exists */) { ++m[dexnr]; });
              local_non_perf_sensitive_insns++;
            }
          }
        }
        if (local_perf_sensitive_insns) {
          perf_sensitive_insns += local_perf_sensitive_insns;
        }
        if (local_non_perf_sensitive_insns) {
          non_perf_sensitive_insns += local_non_perf_sensitive_insns;
        }
      });

  // Also, add all the strings that occurred in perf-sensitive methods
  // to the non_load_strings datastructure, as we won't attempt to dedup them.
  for (const auto& it : perf_sensitive_strings) {
    const auto str = it.first;
    TRACE(DS, 3, "[dedup strings] perf sensitive string: {%s}", SHOW(str));

    const auto& dexes = it.second;
    for (const auto dexnr : dexes) {
      auto& strings = non_load_strings[dexnr];
      strings.emplace(str);
    }
  }

  m_stats.perf_sensitive_strings = perf_sensitive_strings.size();
  m_stats.non_perf_sensitive_strings = occurrences.size();
  m_stats.perf_sensitive_insns = (size_t)perf_sensitive_insns;
  m_stats.non_perf_sensitive_insns = (size_t)non_perf_sensitive_insns;
  return occurrences;
}

std::unordered_map<const DexString*, DedupStrings::DedupStringInfo>
DedupStrings::get_strings_to_dedup(
    DexClassesVector& dexen,
    const ConcurrentMap<const DexString*, std::unordered_map<size_t, size_t>>&
        occurrences,
    std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
    std::unordered_set<const DexMethod*>& perf_sensitive_methods,
    const std::unordered_set<const DexString*> non_load_strings[]) {
  // Use heuristics to determine which strings to dedup, create factory
  // methods as appropriate, and persist relevant information to aid the later
  // rewriting of all const-string instructions.

  std::unordered_map<const DexString*, DedupStrings::DedupStringInfo>
      strings_to_dedup;

  // Do a cost/benefit analysis to figure out which strings to access via
  // factory methods, and where to put to the factory method
  std::vector<const DexString*> strings_in_dexes[dexen.size()];
  std::unordered_set<size_t> hosting_dexnrs;
  std::vector<const DexString*> ordered_strings;
  ordered_strings.reserve(occurrences.size());
  for (auto& p : occurrences) {
    const auto& m = p.second;
    always_assert(!m.empty());
    if (m.size() == 1) continue;
    ordered_strings.push_back(p.first);
  }
  std::sort(ordered_strings.begin(), ordered_strings.end(), compare_dexstrings);
  for (auto* s : ordered_strings) {
    // We are going to look at the situation of a particular string here
    const auto& m = occurrences.at_unsafe(s);
    always_assert(m.size() > 1);
    const auto entry_size = s->get_entry_size();
    const auto get_size_reduction = [entry_size, non_load_strings](
                                        const DexString* str, size_t dexnr,
                                        size_t loads) -> size_t {
      const auto has_non_load_string = non_load_strings[dexnr].count(str) != 0;
      if (has_non_load_string) {
        // If there's a non-load string, there's nothing to gain
        return 0;
      }

      size_t code_size_increase =
          loads * (6 /* invoke */ + 2 /* move-result */);
      if (4 + entry_size < code_size_increase) {
        // If the string itself is taking up less space than the code size
        // increase we would incur when referencing the string via a
        // referenced load method, then there's nothing to gain
        return 0;
      }

      return 4 + entry_size - code_size_increase;
    };

    // First, we identify which dex could and should host the string in
    // its string factory method
    struct HostInfo {
      size_t dexnr;
      size_t size_reduction;
    };
    boost::optional<HostInfo> host_info;
    for (size_t dexnr = 0; dexnr < dexen.size(); ++dexnr) {
      // There's a configurable limit of how many factory methods / hosts we
      // can have in total
      if (hosting_dexnrs.count(dexnr) == 0 &&
          hosting_dexnrs.size() == m_max_factory_methods) {
        // We could try a bit harder to determine the optimal set of hosts,
        // but the best fix in this case is probably to raise the limit
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s} dex #%zu cannot "
              "be used as dedup strings max factory methods limit reached",
              SHOW(s), dexnr);
        ++m_stats.excluded_out_of_factory_methods_strings;
        continue;
      }

      // So this dex could host the current string s
      const auto mit = m.find(dexnr);
      const auto loads = mit == m.end() ? 0 : mit->second;
      // Figure out what the size reduction would be if this dex would *not*
      // be hosting string s, also considering whether we'd keep around a copy
      // of the string in this dex anyway
      const auto size_reduction = get_size_reduction(s, dexnr, loads);
      if (!host_info || size_reduction < host_info->size_reduction) {
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s} dex #%zu can "
              "host with size reduction %zu",
              SHOW(s), dexnr, size_reduction);
        host_info = (HostInfo){dexnr, size_reduction};
      } else {
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s} dex #%zu won't "
              "host due insufficient size reduction %zu",
              SHOW(s), dexnr, size_reduction);
      }
    }

    // We have a zero max_cost if and only if we didn't find any suitable
    // hosting_dexnr
    if (!host_info) {
      TRACE(DS, 3, "[dedup strings] non perf sensitive string: {%s} - no host",
            SHOW(s));
      continue;
    }
    size_t hosting_dexnr = host_info->dexnr;

    // Second, we figure out which other dexes should get their const-string
    // instructions rewritten
    size_t total_size_reduction = 0;
    size_t duplicate_string_loads = 0;
    std::unordered_set<size_t> dexes_to_dedup;
    for (const auto& q : m) {
      const auto dexnr = q.first;
      const auto loads = q.second;
      if (dexnr == hosting_dexnr) {
        continue;
      }

      const auto size_reduction = get_size_reduction(s, dexnr, loads);

      if (non_load_strings[dexnr].count(s) != 0) {
        always_assert(size_reduction == 0);
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s}*%zu is a "
              "non-load string in non-hosting dex #%zu",
              SHOW(s), loads, dexnr);
        ++m_stats.excluded_duplicate_non_load_strings;
        // No point in rewriting const-string instructions for this string
        // in this dex as string will be referenced from this dex anyway
        continue;
      }

      if (size_reduction > 0) {
        duplicate_string_loads += loads;
        total_size_reduction += size_reduction;
        dexes_to_dedup.emplace(dexnr);
      }
    }

    const auto hosting_code_size_increase =
        (4 /* switch-target-offset */ + 4 /* const-string */ + 2 /* return */);

    // Third, we see if there's any overall gain from doing anything about
    // this particular string
    if (total_size_reduction < hosting_code_size_increase) {
      TRACE(DS, 3,
            "[dedup strings] non perf sensitive string: {%s} ignored as %zu < "
            "%u",
            SHOW(s), total_size_reduction, hosting_code_size_increase);
      continue;
    }

    // Yes! We found a string that's worthwhile to dedup.

    if (hosting_dexnrs.count(hosting_dexnr) == 0) {
      hosting_dexnrs.emplace(hosting_dexnr);

      if (hosting_dexnrs.size() == m_max_factory_methods) {
        TRACE(
            DS, 1,
            "[dedup strings] dedup strings max factory methods limit reached; "
            "consider changing configuration to increase limit");
      }
    }

    m_stats.duplicate_strings += dexes_to_dedup.size();
    m_stats.duplicate_strings_size += (4 + entry_size) * dexes_to_dedup.size();
    m_stats.duplicate_string_loads += duplicate_string_loads;
    m_stats.expected_size_reduction +=
        total_size_reduction - hosting_code_size_increase;
    DedupStringInfo dedup_string_info;
    dedup_string_info.duplicate_string_loads = duplicate_string_loads;
    dedup_string_info.dexes_to_dedup = dexes_to_dedup;
    strings_to_dedup.emplace(s, std::move(dedup_string_info));
    strings_in_dexes[hosting_dexnr].push_back(s);

    TRACE(DS, 3,
          "[dedup strings] non perf sensitive string: {%s} is deduped in %zu "
          "dexes, saving %zu string table bytes, transforming %zu string "
          "loads, %zu expected size reduction",
          SHOW(s), dexes_to_dedup.size(),
          (4 + entry_size) * dexes_to_dedup.size(), duplicate_string_loads,
          total_size_reduction - hosting_code_size_increase);
  }

  // Order strings to give more often used strings smaller indices;
  // generate factory methods; remember details in dedup-info data structure
  for (size_t dexnr = 0; dexnr < dexen.size(); ++dexnr) {
    std::vector<const DexString*>& strings = strings_in_dexes[dexnr];
    if (strings.empty()) {
      continue;
    }
    std::sort(
        strings.begin(), strings.end(),
        [&strings_to_dedup](const DexString* a, const DexString* b) -> bool {
          auto a_loads = strings_to_dedup[a].duplicate_string_loads;
          auto b_loads = strings_to_dedup[b].duplicate_string_loads;
          if (a_loads != b_loads) {
            return a_loads > b_loads;
          }
          return dexstrings_comparator()(a, b);
        });
    const auto const_string_method =
        make_const_string_loader_method(dexen.at(dexnr), dexnr, strings);
    always_assert(strings.size() < 0xFFFFFFFF);
    for (uint32_t i = 0; i < strings.size(); i++) {
      auto const s = strings[i];
      auto& info = strings_to_dedup[s];

      TRACE(
          DS, 2,
          "[dedup strings] hosting dex %zu index %u dup-loads %zu string {%s}",
          dexnr, i, info.duplicate_string_loads, SHOW(s));

      redex_assert(info.index == 0xFFFFFFFF);
      redex_assert(info.const_string_method == nullptr);
      info.index = i;
      info.const_string_method = const_string_method;
    }
    methods_to_dex.insert({const_string_method, dexnr});
    perf_sensitive_methods.emplace(const_string_method);
    m_stats.factory_methods++;
  }

  return strings_to_dedup;
}

void DedupStrings::rewrite_const_string_instructions(
    const Scope& scope,
    const std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
    const std::unordered_set<const DexMethod*>& perf_sensitive_methods,
    const std::unordered_map<const DexString*, DedupStrings::DedupStringInfo>&
        strings_to_dedup) {

  walk::parallel::code(
      scope, [&methods_to_dex, &strings_to_dedup,
              &perf_sensitive_methods](DexMethod* method, IRCode& code) {
        if (perf_sensitive_methods.count(method) != 0) {
          // We don't rewrite methods in the primary dex or other perf-sensitive
          // methods.
          return;
        }

        const auto dexnr = methods_to_dex.at(method);

        // First, we collect all const-string instructions that we want to
        // rewrite
        always_assert(code.editable_cfg_built());
        auto& cfg = code.cfg();
        auto ii = cfg::InstructionIterable(cfg);
        std::vector<std::pair<cfg::InstructionIterator, const DedupStringInfo*>>
            const_strings;
        for (auto it = ii.begin(); it != ii.end(); it++) {
          // Do we have a const-string instruction?
          const auto insn = it->insn;
          if (insn->opcode() != OPCODE_CONST_STRING) {
            continue;
          }

          // We we rewrite this particular instruction?
          const auto it2 = strings_to_dedup.find(insn->get_string());
          if (it2 == strings_to_dedup.end()) {
            continue;
          }

          const auto& info = it2->second;
          if (info.dexes_to_dedup.count(dexnr) == 0) {
            continue;
          }

          const_strings.emplace_back(it, &info);
        }

        if (const_strings.empty()) {
          return;
        }

        // Second, we actually rewrite them.
        cfg::CFGMutation cfg_mut(cfg);

        // From
        //   const-string v0, "foo"
        // into
        //   const v1, 123 // index of "foo" in some hosting dex
        //   invoke-static {v1}, $const-string // of hosting dex
        //   move-result-object v0
        // where v1 is a new temp register.

        // Note that it's important to not just re-use the already present
        // register v0, as that would change its type and cause type conflicts
        // in catch blocks, if any.

        auto temp_reg = cfg.allocate_temp();
        for (const auto& p : const_strings) {
          const auto& const_string_it = p.first;
          const auto& info = *p.second;
          auto move_result = cfg.move_result_of(const_string_it);
          always_assert(move_result != ii.end());
          always_assert(
              opcode::is_a_move_result_pseudo(move_result->insn->opcode()));
          const auto reg = move_result->insn->dest();

          std::vector<IRInstruction*> replacements;

          IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
          const_inst->set_dest(temp_reg)->set_literal(info.index);
          replacements.push_back(const_inst);

          IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
          always_assert(info.const_string_method != nullptr);
          invoke_inst->set_method(info.const_string_method)
              ->set_srcs_size(1)
              ->set_src(0, temp_reg);
          replacements.push_back(invoke_inst);

          IRInstruction* move_result_inst =
              new IRInstruction(OPCODE_MOVE_RESULT_OBJECT);
          move_result_inst->set_dest(reg);
          replacements.push_back(move_result_inst);

          cfg_mut.replace(const_string_it, replacements);
        }
        cfg_mut.flush();
      });
}

// In each dex, we might introduce as many new method refs and type refs as we
// might add factory methods. This makes sure that the inter-dex pass keeps
// space for that many method refs and type refs.
class DedupStringsInterDexPlugin : public interdex::InterDexPassPlugin {
 public:
  explicit DedupStringsInterDexPlugin(size_t max_factory_methods)
      : m_max_factory_methods(max_factory_methods) {}

  interdex::ReserveRefsInfo reserve_refs() override {
    return interdex::ReserveRefsInfo(/* frefs */ 0,
                                     /* trefs */ m_max_factory_methods,
                                     /* mrefs */ m_max_factory_methods);
  }

 private:
  size_t m_max_factory_methods;
};

void DedupStringsPass::bind_config() {
  // The dedup-strings transformation introduces new method refs to refer
  // to factory methods. Factory methods are currently only placed into
  // dexes in the first store.
  // There's a limit of how many dexes can be in a store due to the locator
  // scheme we use. See locator.h for more information.

  // TODO: Instead of a user-defined limit, or over-approximating by default,
  // consider running InterDex twice to first get the number of dexes, and then
  // use that number here.
  int64_t default_max_factory_methods =
      (1 << facebook::Locator::dexnr_bits) - 1;
  bind("max_factory_methods", default_max_factory_methods,
       m_max_factory_methods);
  float default_method_profiles_appear_percent_threshold = 1;
  bind("method_profiles_appear_percent_threshold",
       default_method_profiles_appear_percent_threshold,
       m_method_profiles_appear_percent_threshold);
  bind("legacy_perf_logic", false, m_legacy_perf_logic);

  trait(Traits::Pass::unique, true);

  after_configuration([this] {
    always_assert(m_max_factory_methods > 0);
    interdex::InterDexRegistry* registry =
        static_cast<interdex::InterDexRegistry*>(
            PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
    std::function<interdex::InterDexPassPlugin*()> fn =
        [this]() -> interdex::InterDexPassPlugin* {
      return new DedupStringsInterDexPlugin(m_max_factory_methods);
    };
    registry->register_plugin("DEDUP_STRINGS_PLUGIN", std::move(fn));
  });
}

void DedupStringsPass::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  DedupStrings ds(m_max_factory_methods,
                  m_method_profiles_appear_percent_threshold,
                  m_legacy_perf_logic, conf.get_method_profiles());
  ds.run(stores);
  const auto stats = ds.get_stats();
  mgr.incr_metric(METRIC_PERF_SENSITIVE_STRINGS, stats.perf_sensitive_strings);
  mgr.incr_metric(METRIC_NON_PERF_SENSITIVE_STRINGS,
                  stats.non_perf_sensitive_strings);
  TRACE(DS, 1, "[dedup strings] perf sensitive strings: %zu vs %zu",
        stats.perf_sensitive_strings, stats.non_perf_sensitive_strings);

  mgr.incr_metric(METRIC_PERF_SENSITIVE_METHODS, stats.perf_sensitive_methods);
  mgr.incr_metric(METRIC_NON_PERF_SENSITIVE_METHODS,
                  stats.non_perf_sensitive_methods);
  mgr.incr_metric(METRIC_PERF_SENSITIVE_INSNS, stats.perf_sensitive_insns);
  mgr.incr_metric(METRIC_NON_PERF_SENSITIVE_INSNS,
                  stats.non_perf_sensitive_insns);
  TRACE(DS, 1,
        "[dedup strings] perf sensitive methods (instructions): %zu(%zu) vs "
        "%zu(%zu)",
        stats.perf_sensitive_methods, stats.perf_sensitive_insns,
        stats.non_perf_sensitive_methods, stats.non_perf_sensitive_insns);

  mgr.incr_metric(METRIC_DUPLICATE_STRINGS, stats.duplicate_strings);
  mgr.incr_metric(METRIC_DUPLICATE_STRINGS_SIZE, stats.duplicate_strings_size);
  mgr.incr_metric(METRIC_DUPLICATE_STRING_LOADS, stats.duplicate_string_loads);
  mgr.incr_metric(METRIC_EXPECTED_SIZE_REDUCTION,
                  stats.expected_size_reduction);
  mgr.incr_metric(METRIC_DEXES_WITHOUT_HOST, stats.dexes_without_host_cls);
  mgr.incr_metric(METRIC_EXCLUDED_DUPLICATE_NON_LOAD_STRINGS,
                  stats.excluded_duplicate_non_load_strings);
  mgr.incr_metric(METRIC_FACTORY_METHODS, stats.factory_methods);
  mgr.incr_metric(METRIC_EXCLUDED_OUT_OF_FACTORY_METHODS_STRINGS,
                  stats.excluded_out_of_factory_methods_strings);
  TRACE(DS, 1,
        "[dedup strings] duplicate strings: %zu, size: %zu, loads: %zu; "
        "expected size reduction: %zu; "
        "dexes without host: %zu; "
        "excluded duplicate non-load strings: %zu; factory methods: %zu; "
        "excluded out of factory methods strings: %zu",
        stats.duplicate_strings, stats.duplicate_strings_size,
        stats.duplicate_string_loads, stats.expected_size_reduction,
        stats.dexes_without_host_cls, stats.excluded_duplicate_non_load_strings,
        stats.factory_methods, stats.excluded_out_of_factory_methods_strings);
}

static DedupStringsPass s_pass;
