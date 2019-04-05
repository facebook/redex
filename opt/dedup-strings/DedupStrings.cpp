/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DedupStrings.h"

#include <vector>

#include "ConcurrentContainers.h"
#include "Creators.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Walkers.h"
#include "locator.h"

namespace {

constexpr const char* METRIC_STRINGS_WITHIN_TRY = "num_strings_within_try";
constexpr const char* METRIC_PERF_SENSITIVE_STRINGS =
    "num_perf_sensitive_strings";
constexpr const char* METRIC_NON_PERF_SENSITIVE_STRINGS =
    "num_non_perf_sensitive_strings";
constexpr const char* METRIC_PERF_SENSITIVE_METHODS =
    "num_perf_sensitive_methods";
constexpr const char* METRIC_NON_PERF_SENSITIVE_METHODS =
    "num_non_perf_sensitive_methods";
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
  for (size_t i = 0; i < dexen.size(); i++) {
    auto& strings = non_load_strings[i];
    gather_non_load_strings(dexen[i], &strings);
    // We just add strings appearing in try-blocks here, as we won't bother
    // deduplicating them.
    // TODO: Consider generalizing the later code transformation to enable
    // passing the verifier for string loads appearing in try-blocks.
    gather_load_strings_within_try(dexen[i], &strings);
  }

  // For each string, figure out how many times it's loaded per dex
  ConcurrentMap<DexString*, std::unordered_map<size_t, size_t>> occurrences =
      get_occurrences(scope, methods_to_dex, perf_sensitive_methods,
                      non_load_strings);

  // Use heuristics to determine which strings to dedup,
  // and figure out factory method details
  std::unordered_map<DexString*, DedupStrings::DedupStringInfo>
      strings_to_dedup =
          get_strings_to_dedup(dexen, occurrences, methods_to_dex,
                               perf_sensitive_methods, non_load_strings);

  // Rewrite const-string instructions
  rewrite_const_string_instructions(scope, methods_to_dex,
                                    perf_sensitive_methods, strings_to_dedup);
}

std::vector<DexClass*> DedupStrings::get_host_classes(DexClassesVector& dexen) {
  // For dex, let's find an existing class into which we can add our
  // $const$string factory method.
  // We'd prefer one without a class initializer.

  std::vector<DexClass*> host_classes;
  for (auto& classes : dexen) {
    DexClass* host_cls = nullptr;
    for (auto cls : classes) {
      // Non-root classes shouldn't participate in reflection, and so adding
      // things to them should be benign.
      if (!root(cls) && is_public(cls) && !cls->is_external() &&
          !is_interface(cls)) {
        if (host_cls == nullptr) {
          host_cls = cls;
        }
        if (!cls->get_clinit()) {
          // Stop here if we found a suitable class without a class initializer
          break;
        }
      }
    }

    if (host_cls) {
      TRACE(DS, 2, "[dedup strings] host class in dex #%u is {%s}\n",
            host_classes.size(), SHOW(host_cls));
    } else {
      TRACE(DS, 2, "[dedup strings] no host class in dex #%u\n",
            host_classes.size());
      ++m_stats.dexes_without_host_cls;
    }
    host_classes.push_back(host_cls);
  }

  return host_classes;
}

std::unordered_set<const DexMethod*> DedupStrings::get_perf_sensitive_methods(
    const DexClassesVector& dexen) {
  std::unordered_set<const DexMethod*> perf_sensitive_methods;
  auto has_weight = [&](DexMethod* method) -> bool {
    return !m_use_method_to_weight ||
           get_method_weight_if_available(method, &m_method_to_weight);
  };
  for (size_t dexnr = 0; dexnr < dexen.size(); dexnr++) {
    auto& classes = dexen[dexnr];
    for (auto cls : classes) {
      auto process_method = [&](DexMethod* method) {
        if (method->get_code() != nullptr) {
          // All methods in the primary dex 0 must not be touched,
          // as well as methods marked as being perf-sensitive
          bool perf_sensitive =
              dexnr == 0 || (cls->is_perf_sensitive() && has_weight(method));
          if (perf_sensitive) {
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
    DexClass* host_cls, const std::vector<DexString*>& strings) {
  // Here we build the string factory method with a big switch statement.
  always_assert(strings.size() > 0);
  const auto string_type = get_string_type();
  const auto proto = DexProto::make_proto(
      string_type, DexTypeList::make_type_list({get_int_type()}));
  MethodCreator method_creator(host_cls->get_type(),
                               DexString::make_string("$const$string"),
                               proto,
                               ACC_PUBLIC | ACC_STATIC);
  redex_assert(strings.size() > 0);
  auto id_arg = method_creator.get_local(0);
  auto res_var = method_creator.make_local(get_string_type());
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
  return method;
}

void DedupStrings::gather_non_load_strings(
    DexClasses& classes, std::unordered_set<const DexString*>* strings) {
  // Let's figure out the set of "non-load" strings, i.e. the strings which
  // are referenced by some metadata (and not just const-string instructions)
  std::vector<DexString*> lstring;
  std::vector<DexType*> ltype;
  std::vector<DexFieldRef*> lfield;
  std::vector<DexMethodRef*> lmethod;
  gather_components(lstring, ltype, lfield, lmethod, classes,
                    /* exclude_loads */ true);

  strings->insert(lstring.begin(), lstring.end());
}

void DedupStrings::gather_load_strings_within_try(
    DexClasses& classes, std::unordered_set<const DexString*>* strings) {
  // The code transformation we do later, turning string-loads into
  // static function calls, is not valid within try blocks.
  // To work around this rare issue, we pick up all strings here that
  // occur in such try blocks. They will be excluded from the optimization,
  // similar to "non-load" strings.
  ConcurrentSet<DexString*> load_strings_within_try;
  walk::parallel::code(
      classes, [&load_strings_within_try](DexMethod* method, IRCode& code) {
        int in_try = 0;
        for (auto& mie : code) {
          if (mie.type == MFLOW_OPCODE) {
            if (in_try && mie.insn->opcode() == OPCODE_CONST_STRING) {
              load_strings_within_try.emplace(mie.insn->get_string());
            }
          } else if (mie.type == MFLOW_TRY) {
            auto& tentry = mie.tentry;
            if (tentry->type == TRY_START) {
              always_assert(!in_try);
              in_try = true;
            } else if (tentry->type == TRY_END) {
              always_assert(in_try);
              in_try = false;
            } else {
              always_assert(false);
            }
          }
        }
      });

  m_stats.load_strings_within_try += load_strings_within_try.size();
  strings->insert(load_strings_within_try.begin(),
                  load_strings_within_try.end());
}

ConcurrentMap<DexString*, std::unordered_map<size_t, size_t>>
DedupStrings::get_occurrences(
    const Scope& scope,
    const std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
    const std::unordered_set<const DexMethod*>& perf_sensitive_methods,
    std::unordered_set<const DexString*> non_load_strings[]) {
  // For each string, figure out how many times it's loaded per dex
  ConcurrentMap<DexString*, std::unordered_map<size_t, size_t>> occurrences;
  ConcurrentMap<DexString*, std::unordered_set<size_t>> perf_sensitive_strings;
  walk::parallel::code(
      scope, [&occurrences, &perf_sensitive_strings, &methods_to_dex,
              &perf_sensitive_methods](DexMethod* method, IRCode& code) {
        const auto dexnr = methods_to_dex.at(method);
        const auto perf_sensitive = perf_sensitive_methods.count(method) != 0;
        for (auto& mie : InstructionIterable(code)) {
          const auto insn = mie.insn;
          if (insn->opcode() == OPCODE_CONST_STRING) {
            const auto str = insn->get_string();
            if (perf_sensitive) {
              perf_sensitive_strings.update(
                  str,
                  [dexnr](const DexString*,
                          std::unordered_set<size_t>& s,
                          bool /* exists */) { s.emplace(dexnr); });
            } else {
              occurrences.update(str,
                                 [dexnr](const DexString*,
                                         std::unordered_map<size_t, size_t>& m,
                                         bool /* exists */) { ++m[dexnr]; });
            }
          }
        }
      });

  // Also, add all the strings that occurred in perf-sensitive methods
  // to the non_load_strings datastructure, as we won't attempt to dedup them.
  for (const auto& it : perf_sensitive_strings) {
    const auto str = it.first;
    TRACE(DS, 3, "[dedup strings] perf sensitive string: {%s}\n", SHOW(str));

    const auto& dexes = it.second;
    for (const auto dexnr : dexes) {
      auto& strings = non_load_strings[dexnr];
      strings.emplace(str);
    }
  }

  m_stats.perf_sensitive_strings = perf_sensitive_strings.size();
  m_stats.non_perf_sensitive_strings = occurrences.size();
  return occurrences;
}

std::unordered_map<DexString*, DedupStrings::DedupStringInfo>
DedupStrings::get_strings_to_dedup(
    DexClassesVector& dexen,
    const ConcurrentMap<DexString*, std::unordered_map<size_t, size_t>>&
        occurrences,
    std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
    std::unordered_set<const DexMethod*>& perf_sensitive_methods,
    const std::unordered_set<const DexString*> non_load_strings[]) {
  // Use heuristics to determine which strings to dedup, create factory
  // methods as appropriate, and persist relevant information to aid the later
  // rewriting of all const-string instructions.

  std::unordered_map<DexString*, DedupStrings::DedupStringInfo>
      strings_to_dedup;

  const std::vector<DexClass*> host_classes = get_host_classes(dexen);

  // Do a cost/benefit analysis to figure out which strings to access via
  // factory methods, and where to put to the factory method
  std::vector<DexString*> strings_in_dexes[dexen.size()];
  std::unordered_set<size_t> hosting_dexnrs;
  for (const auto& p : occurrences) {
    // We are going to look at the situation of a particular string here
    const auto& m = p.second;
    always_assert(m.size() >= 1);
    if (m.size() == 1) continue;
    const auto s = p.first;
    const auto entry_size = s->get_entry_size();
    const auto get_size_reduction = [entry_size, non_load_strings](
                                        DexString* str, size_t dexnr,
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
      // We need a host class to host
      if (!host_classes[dexnr]) {
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s} dex #%u has no "
              "host\n",
              SHOW(s), dexnr);
        continue;
      }

      // There's a configurable limit of how many factory methods / hosts we
      // can have in total
      if (hosting_dexnrs.count(dexnr) == 0 &&
          hosting_dexnrs.size() == m_max_factory_methods) {
        // We could try a bit harder to determine the optimal set of hosts,
        // but the best fix in this case is probably to raise the limit
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s} dex #%u cannot "
              "be used as dedup strings max factory methods limit reached\n",
              SHOW(s), dexnr);
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
              "[dedup strings] non perf sensitive string: {%s} dex #%u can "
              "host with size reduction %u\n",
              SHOW(s), dexnr, size_reduction);
        host_info = (HostInfo){dexnr, size_reduction};
      } else {
        TRACE(DS, 4,
              "[dedup strings] non perf sensitive string: {%s} dex #%u won't "
              "host due insufficient size reduction %u\n",
              SHOW(s), dexnr, size_reduction);
      }
    }

    // We have a zero max_cost if and only if we didn't find any suitable
    // hosting_dexnr
    if (!host_info) {
      TRACE(DS, 3,
            "[dedup strings] non perf sensitive string: {%s} - no host\n",
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
              "[dedup strings] non perf sensitive string: {%s}*%u is a "
              "non-load string in non-hosting dex #%u\n",
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
            "[dedup strings] non perf sensitive string: {%s} ignored as %u < "
            "%u\n",
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
            "consider changing configuration to increase limit\n");
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

    TRACE(
        DS, 3,
        "[dedup strings] non perf sensitive string: {%s} is deduped in %u "
        "dexes, saving %u string table bytes, transforming %u string loads, %u "
        "expected size reduction\n",
        SHOW(s), dexes_to_dedup.size(),
        (4 + entry_size) * dexes_to_dedup.size(), duplicate_string_loads,
        total_size_reduction - hosting_code_size_increase);
  }

  // Order strings to give more often used strings smaller indices;
  // generate factory methods; remember details in dedup-info data structure
  for (size_t dexnr = 0; dexnr < dexen.size(); ++dexnr) {
    std::vector<DexString*>& strings = strings_in_dexes[dexnr];
    if (strings.size() == 0) {
      continue;
    }
    std::sort(strings.begin(), strings.end(),
              [&strings_to_dedup](DexString* a, DexString* b) -> bool {
                return strings_to_dedup[a].duplicate_string_loads >
                       strings_to_dedup[b].duplicate_string_loads;
              });
    const auto const_string_method =
        make_const_string_loader_method(host_classes[dexnr], strings);
    always_assert(strings.size() < 0xFFFFFFFF);
    for (uint32_t i = 0; i < strings.size(); i++) {
      auto const s = strings[i];
      auto& info = strings_to_dedup[s];

      TRACE(
          DS, 2,
          "[dedup strings] hosting dex %u index %u dup-loads %u string {%s}\n",
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
    const std::unordered_map<DexString*, DedupStrings::DedupStringInfo>&
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
        const auto ii = InstructionIterable(code);
        std::vector<std::pair<IRInstruction*, uint16_t>> const_strings;
        for (auto it = ii.begin(); it != ii.end(); it++) {
          // do we have a sequence of const-string + move-pseudo-result
          // instruction?
          const auto insn = it->insn;
          if (insn->opcode() != OPCODE_CONST_STRING) {
            continue;
          }
          auto move_result_pseudo = ir_list::move_result_pseudo_of(it.unwrap());

          const_strings.push_back({insn, move_result_pseudo->dest()});
        }

        // Second, we actually rewrite them.

        // From
        //   const-string v0, "foo"
        // into
        //   const v0, 123 // index of "foo" in some hosting dex
        //   invoke-static {v0}, $const-string // of hosting dex
        //   move-result-object v0
        for (const auto& p : const_strings) {
          const auto const_string = p.first;
          const auto reg = p.second;

          const auto it = strings_to_dedup.find(const_string->get_string());
          if (it == strings_to_dedup.end()) {
            continue;
          }
          const auto& info = it->second;
          if (info.dexes_to_dedup.count(dexnr) == 0) {
            continue;
          }

          std::vector<IRInstruction*> replacements;

          IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
          const_inst->set_literal(info.index);
          const_inst->set_dest(reg);
          replacements.push_back(const_inst);

          IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
          always_assert(info.const_string_method != nullptr);
          invoke_inst->set_method(info.const_string_method);
          invoke_inst->set_arg_word_count(1);
          invoke_inst->set_src(0, reg);
          replacements.push_back(invoke_inst);

          IRInstruction* move_result_inst =
              new IRInstruction(OPCODE_MOVE_RESULT_OBJECT);
          move_result_inst->set_dest(reg);
          replacements.push_back(move_result_inst);

          // TODO: replace_opcode takes linear time!
          code.replace_opcode(const_string, replacements);
        }
      });
}

class DedupStringsInterDexPlugin : public interdex::InterDexPassPlugin {
 public:
  DedupStringsInterDexPlugin(size_t max_factory_methods)
      : m_max_factory_methods(max_factory_methods) {}
  void configure(const Scope& scope, ConfigFiles& cfg) override{};
  bool should_skip_class(const DexClass* clazz) override { return false; }
  void gather_refs(const interdex::DexInfo& dex_info,
                   const DexClass* cls,
                   std::vector<DexMethodRef*>& mrefs,
                   std::vector<DexFieldRef*>& frefs,
                   std::vector<DexType*>& trefs,
                   std::vector<DexClass*>* erased_classes,
                   bool should_not_relocate_methods_of_class) override {}
  size_t reserve_mrefs() override {
    // In each, we might introduce as many new method refs are we might add
    // factory methods. This makes sure that the inter-dex pass keeps space for
    // that many method refs.
    return m_max_factory_methods;
  }
  DexClasses additional_classes(const DexClassesVector& outdex,
                                const DexClasses& classes) override {
    DexClasses res;
    return res;
  }
  void cleanup(const std::vector<DexClass*>& scope) override {}

 private:
  size_t m_max_factory_methods;
};

void DedupStringsPass::configure_pass(const JsonWrapper& jw) {
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
  int64_t max_factory_methods;
  jw.get("max_factory_methods", default_max_factory_methods,
         max_factory_methods);
  always_assert(max_factory_methods > 0);
  m_max_factory_methods = max_factory_methods;

  bool use_method_to_weight;
  jw.get("use_method_to_weight", false, use_method_to_weight);
  m_use_method_to_weight = use_method_to_weight;

  interdex::InterDexRegistry* registry =
      static_cast<interdex::InterDexRegistry*>(
          PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
  std::function<interdex::InterDexPassPlugin*()> fn =
      [max_factory_methods]() -> interdex::InterDexPassPlugin* {
    return new DedupStringsInterDexPlugin(max_factory_methods);
  };
  registry->register_plugin("DEDUP_STRINGS_PLUGIN", std::move(fn));
}

void DedupStringsPass::run_pass(DexStoresVector& stores,
                                ConfigFiles& cfg,
                                PassManager& mgr) {
  DedupStrings ds(m_max_factory_methods, m_use_method_to_weight,
                  cfg.get_method_to_weight());
  ds.run(stores);
  const auto stats = ds.get_stats();
  mgr.incr_metric(METRIC_STRINGS_WITHIN_TRY, stats.load_strings_within_try);
  TRACE(DS, 1, "[dedup strings] load strings within try: %u\n",
        stats.load_strings_within_try);
  mgr.incr_metric(METRIC_PERF_SENSITIVE_STRINGS, stats.perf_sensitive_strings);
  mgr.incr_metric(METRIC_NON_PERF_SENSITIVE_STRINGS,
                  stats.non_perf_sensitive_strings);
  TRACE(DS, 1, "[dedup strings] perf sensitive strings: %u vs %u\n",
        stats.perf_sensitive_strings, stats.non_perf_sensitive_strings);

  mgr.incr_metric(METRIC_PERF_SENSITIVE_METHODS, stats.perf_sensitive_methods);
  mgr.incr_metric(METRIC_NON_PERF_SENSITIVE_METHODS,
                  stats.non_perf_sensitive_methods);
  TRACE(DS, 1, "[dedup strings] perf sensitive methods: %u vs %u\n",
        stats.perf_sensitive_methods, stats.non_perf_sensitive_methods);

  mgr.incr_metric(METRIC_DUPLICATE_STRINGS, stats.duplicate_strings);
  mgr.incr_metric(METRIC_DUPLICATE_STRINGS_SIZE, stats.duplicate_strings_size);
  mgr.incr_metric(METRIC_DUPLICATE_STRING_LOADS, stats.duplicate_string_loads);
  mgr.incr_metric(METRIC_EXPECTED_SIZE_REDUCTION,
                  stats.expected_size_reduction);
  mgr.incr_metric(METRIC_DEXES_WITHOUT_HOST, stats.dexes_without_host_cls);
  mgr.incr_metric(METRIC_EXCLUDED_DUPLICATE_NON_LOAD_STRINGS,
                  stats.excluded_duplicate_non_load_strings);
  mgr.incr_metric(METRIC_FACTORY_METHODS, stats.factory_methods);
  TRACE(DS, 1,
        "[dedup strings] duplicate strings: %u, size: %u, loads: %u; "
        "expected size reduction: %u; "
        "dexes without host: %u; "
        "excluded duplicate non-load strings: %u; factory methods: %u\n",
        stats.duplicate_strings, stats.duplicate_strings_size,
        stats.duplicate_string_loads, stats.expected_size_reduction,
        stats.dexes_without_host_cls, stats.excluded_duplicate_non_load_strings,
        stats.factory_methods);
}

static DedupStringsPass s_pass;
