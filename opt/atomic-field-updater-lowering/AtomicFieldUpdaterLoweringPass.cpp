/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AtomicFieldUpdaterLoweringPass.h"

#include "AtomicFieldUpdaters.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "LiveRange.h"
#include "PassManager.h"
#include "ReflectionAnalysis.h"
#include "Show.h"
#include "Trace.h"
#include "TypeInference.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {
using atomic_field_updaters::Kind;
using atomic_field_updaters::kind_name;

// A recognized updater: a `static final Atomic*FieldUpdater` field whose sole
// write is `newUpdater(holder, [T,] "field_name")` in the holder's <clinit>,
// and where `holder.field_name` is a non-static volatile field.
struct UpdaterInfo {
  DexField* updater; // the static final updater field
  DexType* holder; // class declaring the volatile field
  const DexString* field_name; // the volatile field's name
  const DexType* field_type{nullptr}; // the volatile field's declared type
  Kind kind{Kind::REFERENCE};
};

bool is_volatile_instance_field(const DexField* f) {
  return !is_static(f) && is_volatile(f);
}

// How many updaters were recognized, per flavor, indexed by `Kind`. Sized off
// `all_kinds` so a fourth flavor could not leave the array behind.
using KindCounts =
    std::array<size_t,
               std::tuple_size_v<decltype(atomic_field_updaters::all_kinds())>>;

// Recognition is confined to a single <clinit>: an updater is accepted only
// when the class that declares it also declares the volatile field it targets.
// A cross-class updater would need the writing and target classes analyzed
// together, which this does not attempt.
//
// All flavors are handled in one walk. `ReflectionAnalysis` and the use-def
// chains are the expensive part and are per-<clinit>, not per-flavor: a class
// declaring both an Integer and a Long updater -- the common shape, since
// atomicfu emits one per volatile field -- would otherwise rebuild both for
// each flavor in turn.
std::vector<UpdaterInfo> find_updaters(const Scope& scope,
                                       KindCounts& per_kind) {
  std::vector<UpdaterInfo> result;
  per_kind.fill(0);
  const auto* new_updater = atomic_field_updaters::new_updater_name();
  const auto kinds = atomic_field_updaters::present_kinds();
  if (new_updater == nullptr || kinds.empty()) {
    return result;
  }

  // What a `newUpdater` call produced, keyed by the invoke itself --
  // `MoveAwareChains` looks through the `move-result`, so the invoke is what
  // its chains name as the def.
  struct Produced {
    DexType* holder;
    const DexString* name;
    Kind kind;
  };

  for (auto* cls : scope) {
    // Candidate updater fields: static final, of an updater type, carrying the
    // flavor that type implies.
    UnorderedMap<DexField*, Kind> candidates;
    for (auto* f : cls->get_sfields()) {
      auto kit = kinds.find(f->get_type());
      if (kit != kinds.end() && is_final(f)) {
        candidates.emplace(f, kit->second);
      }
    }
    if (candidates.empty()) {
      continue;
    }
    auto* clinit = cls->get_clinit();
    if (clinit == nullptr || clinit->get_code() == nullptr) {
      continue;
    }
    auto& cfg = clinit->get_code()->cfg();
    reflection::ReflectionAnalysis analysis(clinit);

    UnorderedMap<const IRInstruction*, Produced> produced_by;
    // Every store into a candidate field. A field written more than once is
    // rejected below rather than resolved to one of the writes: the lowering
    // substitutes a single offset for a single field, so two writes naming
    // different fields would leave one of them addressed through the other's
    // offset -- a raw memory access at the wrong address, not an exception.
    UnorderedMap<DexField*, std::vector<IRInstruction*>> stores;
    // Fields in the order their first store appears, so that what this returns
    // -- and with it the order offset fields are synthesized in later -- does
    // not depend on hash iteration order.
    std::vector<DexField*> store_order;

    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto* insn = mie.insn;
      auto op = insn->opcode();

      if (opcode::is_invoke_static(op) &&
          insn->get_method()->get_name() == new_updater) {
        auto kit = kinds.find(insn->get_method()->get_class());
        if (kit == kinds.end()) {
          continue;
        }
        const uint16_t name_arg =
            atomic_field_updaters::field_name_arg_index(kit->second);
        if (insn->srcs_size() <= name_arg) {
          continue;
        }
        const auto& cls_obj = analysis.get_abstract_object(insn->src(0), insn);
        const auto& name_obj =
            analysis.get_abstract_object(insn->src(name_arg), insn);
        if (cls_obj && cls_obj->is_class() && cls_obj->dex_type != nullptr &&
            name_obj && name_obj->is_string() &&
            name_obj->dex_string != nullptr) {
          produced_by.emplace(insn,
                              Produced{const_cast<DexType*>(cls_obj->dex_type),
                                       name_obj->dex_string, kit->second});
        }
        continue;
      }

      if (opcode::is_sput_object(op) && insn->get_field()->is_def()) {
        auto* fdef = insn->get_field()->as_def();
        if (candidates.count(fdef) != 0) {
          if (stores[fdef].empty()) {
            store_order.push_back(fdef);
          }
          stores[fdef].push_back(insn);
        }
      }
    }
    if (store_order.empty()) {
      continue;
    }

    // Use-def rather than a scan over the instruction stream: the store and the
    // call that produced its value need not share a block -- a try region, or a
    // value computed under a branch, separates them -- and a register may be
    // reused for something else in between.
    live_range::MoveAwareChains chains(cfg);
    auto use_defs = chains.get_use_def_chains();

    for (auto* fdef : store_order) {
      const auto& sputs = stores.at(fdef);
      if (sputs.size() != 1) {
        TRACE(ATOMUP, 2, "skipping %s: %zu writes in <clinit>", SHOW(fdef),
              sputs.size());
        continue;
      }
      auto def_it = use_defs.find(live_range::Use{sputs.front(), 0});
      if (def_it == use_defs.end() || def_it->second.size() != 1) {
        continue;
      }
      auto info_it = produced_by.find(*def_it->second.begin());
      if (info_it == produced_by.end()) {
        continue;
      }
      const Produced& produced = info_it->second;
      const Kind kind = candidates.at(fdef);
      // The flavor that produced the value must be the flavor of the field it
      // is stored into, or the argument positions it was read with were the
      // wrong ones.
      if (produced.kind != kind) {
        continue;
      }
      // Require the updater to target a field of its own declaring class
      // (the atomicfu shape); cross-class holders are not handled.
      if (produced.holder != cls->get_type()) {
        continue;
      }
      DexField* target = nullptr;
      for (auto* ifield : cls->get_ifields()) {
        if (ifield->get_name() == produced.name &&
            is_volatile_instance_field(ifield)) {
          target = ifield;
          break;
        }
      }
      if (target == nullptr) {
        continue;
      }
      // The named field must have the type the flavor implies. The reference
      // flavor is checked too, rather than left unconstrained: javac would not
      // produce a reference updater over an `int`, but recognition should not
      // rest on the dex having come from javac.
      const auto* target_type = target->get_type();
      const bool type_matches_flavor =
          kind == Kind::INTEGER ? target_type == type::_int()
          : kind == Kind::LONG  ? target_type == type::_long()
                                : type::is_object(target_type);
      if (!type_matches_flavor) {
        continue;
      }
      per_kind.at(static_cast<size_t>(kind))++;
      result.push_back(UpdaterInfo{fdef, produced.holder, produced.name,
                                   target->get_type(), kind});
      TRACE(ATOMUP, 3, "recognized %s updater %s for %s.%s", kind_name(kind),
            SHOW(fdef), SHOW(produced.holder), SHOW(produced.name));
    }
  }
  return result;
}

// Count operation call sites on the updater types, keyed by flavor and
// operation name. Sizes the opportunity independently of how much of it the
// recognition above can actually reach.
void census_ops(const Scope& scope, PassManager& mgr) {
  const auto kinds = atomic_field_updaters::present_kinds();
  if (kinds.empty()) {
    // Still report the total. A program with no updaters has zero operations,
    // which is not the same thing as the metric being missing -- absent, it is
    // indistinguishable from the pass not having run at all.
    mgr.set_metric("ops_total", 0);
    return;
  }
  // Every method in the program is visited, so this walks in parallel, and the
  // counters are keyed by the method ref -- a pointer hash, with no string
  // built at any call site. `AtomicMap` accumulates them without a lock.
  AtomicMap<const DexMethodRef*, size_t> per_operation;
  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    for (auto& mie : cfg::InstructionIterable(code.cfg())) {
      auto* insn = mie.insn;
      if (!opcode::is_invoke_virtual(insn->opcode())) {
        continue;
      }
      const auto* mref = insn->get_method();
      if (kinds.count(mref->get_class()) == 0u ||
          !atomic_field_updaters::is_operation_name(mref->get_name()->str())) {
        continue;
      }
      per_operation.fetch_add(mref, 1);
    }
  });
  // One label per distinct operation rather than per call site, sorted so the
  // trace reads the same way on every run. The proto is part of the label so
  // that overloads stay distinct here too: keying the counters by method ref
  // would buy nothing if the report merged them back together by name.
  //
  // Sorted by the derived label rather than by the container's key, so this is
  // a build-and-sort rather than `unordered_to_ordered`; nothing is summed,
  // because one method ref produces exactly one label.
  size_t total = 0;
  for (auto&& [mref, n] : UnorderedIterable(per_operation)) {
    total += n.load();
  }
  mgr.set_metric("ops_total", total);

  // Everything below is for the trace and nothing else. The per-operation
  // breakdown is not a metric: which rows exist depends on which operations the
  // program happens to call, so two builds being compared would not have the
  // same set. Metrics are for counters whose cardinality is fixed.
  if (traceEnabled(ATOMUP, 2)) {
    std::vector<std::pair<std::string, size_t>> counts;
    counts.reserve(per_operation.size());
    for (auto&& [mref, n] : UnorderedIterable(per_operation)) {
      auto it = kinds.find(mref->get_class());
      counts.emplace_back(std::string("ops_") + kind_name(it->second) + "_" +
                              mref->get_name()->str_copy() +
                              show(mref->get_proto()),
                          n.load());
    }
    std::sort(counts.begin(), counts.end());
    for (const auto& [name, n] : counts) {
      TRACE(ATOMUP, 2, "%s = %zu", name.c_str(), n);
    }
  }
}

// Evaluate every updater operation call site against the obligations that
// lowering it to `sun.misc.Unsafe` must discharge, and report how many pass.
// This is analysis only: it counts what could be lowered without changing it.
//
// Three obligations, all required:
//   1. the receiver resolves to a known updater field -- without one there is
//      no offset to substitute;
//   2. the holder is a subtype of the updater's declaring class, and non-null;
//   3. for reference operations that write a value, the value is a subtype of
//      the field type.
//
// Obligation 2 is not pedantry. `accessCheck` throws ClassCastException for a
// null or wrong-typed holder, whereas `Unsafe` would perform a raw memory
// access at that address -- undefined behaviour rather than an exception. The
// checks may only be dropped once they are proven redundant.
//
// Nullness is tracked separately from the type test, because a site blocked
// only on nullness remains reachable by emitting a runtime check; counting it
// as blocked would understate the opportunity.

// Does every argument after the holder carry a value of the flavor's type?
// True for the operations this pass models (`get`, `set`, `compareAndSet` and
// friends); false for the functional-style ones, whose trailing argument is a
// `UnaryOperator`/`BinaryOperator` rather than a value.
bool writes_only_values(const DexMethodRef* mref, Kind kind) {
  const auto* value_type = atomic_field_updaters::value_type(kind);
  const auto* args = mref->get_proto()->get_args();
  bool holder = true;
  for (const auto* arg : *args) {
    if (holder) {
      holder = false;
      continue;
    }
    if (arg != value_type) {
      return false;
    }
  }
  return true;
}

void analyze_calls(const Scope& scope,
                   const UnorderedMap<DexField*, const UpdaterInfo*>& by_field,
                   PassManager& mgr) {
  const auto updater_kinds = atomic_field_updaters::present_kinds();
  if (updater_kinds.empty()) {
    return;
  }

  // Counted per thread and summed after the walk. The scan below visits every
  // instruction in the program -- the check for "does this method touch an
  // updater at all" cannot be answered more cheaply -- so it runs in parallel,
  // and shared counters would need synchronising on the hot path.
  struct Stats {
    // Sites passing every obligation, keyed by the operation's method ref and
    // whether the holder was proven non-null. A pointer and a flag: no string
    // is built on the hot path, and overloads stay distinct.
    std::map<std::pair<const DexMethodRef*, bool>, size_t> feasible;
    size_t skipped_unresolved{0};
    size_t skipped_unproven{0};
    size_t blocked_holder_type{0};
    size_t blocked_value_type{0};
    size_t blocked_unmodeled_op{0};
    // Breakdown of why resolution failed, to tell a fixable receiver pattern
    // apart from a fundamentally untrackable one.
    size_t no_defs{0};
    size_t def_not_sget{0};
    size_t field_not_def{0};
    size_t field_unknown{0};
    size_t conflicting_defs{0};

    Stats& operator+=(const Stats& that) {
      for (const auto& [key, n] : that.feasible) {
        feasible[key] += n;
      }
      skipped_unresolved += that.skipped_unresolved;
      skipped_unproven += that.skipped_unproven;
      blocked_holder_type += that.blocked_holder_type;
      blocked_value_type += that.blocked_value_type;
      blocked_unmodeled_op += that.blocked_unmodeled_op;
      no_defs += that.no_defs;
      def_not_sget += that.def_not_sget;
      field_not_def += that.field_not_def;
      field_unknown += that.field_unknown;
      conflicting_defs += that.conflicting_defs;
      return *this;
    }
  };

  auto totals = walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
    Stats stats;
    auto* code = method->get_code();
    if (code == nullptr || method->rstate.no_optimizations()) {
      return stats;
    }
    always_assert(code->cfg_built());
    auto& cfg = code->cfg();

    bool any = false;
    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto* insn = mie.insn;
      if (opcode::is_invoke_virtual(insn->opcode()) &&
          updater_kinds.count(insn->get_method()->get_class()) != 0u) {
        any = true;
        break;
      }
    }
    if (!any) {
      return stats;
    }

    type_inference::TypeInference ti(cfg);
    ti.run(method);
    const auto& envs = ti.get_type_environments();

    // Move-aware use-def chains resolve the receiver and the holder across
    // basic blocks: a load hoisted out of a loop lands in a different block
    // from the call that uses it.
    live_range::MoveAwareChains chains(cfg);
    auto use_defs = chains.get_use_def_chains();

    // The receiver `this` of an instance method is non-null on entry -- the
    // invoke that got us here would already have thrown otherwise -- but
    // TypeInference does not encode that. Remember its defining load-param.
    IRInstruction* receiver_insn = nullptr;
    if (!is_static(method)) {
      auto params = cfg.get_param_instructions();
      auto first = params.begin();
      if (first != params.end() &&
          opcode::is_load_param_object(first->insn->opcode())) {
        receiver_insn = first->insn;
      }
    }

    auto resolve_updater = [&](IRInstruction* insn) -> const UpdaterInfo* {
      auto it = use_defs.find(live_range::Use{insn, 0});
      if (it == use_defs.end() || it->second.empty()) {
        stats.skipped_unresolved++;
        stats.no_defs++;
        return nullptr;
      }
      const UpdaterInfo* info = nullptr;
      for (auto* def : it->second) {
        const UpdaterInfo* cand = nullptr;
        DexField* fdef = nullptr;
        if (opcode::is_sget_object(def->opcode())) {
          if (def->get_field()->is_def()) {
            fdef = def->get_field()->as_def();
          } else {
            stats.field_not_def++;
          }
        } else {
          stats.def_not_sget++;
          TRACE(ATOMUP, 4, "unresolved (def not sget-object): def=[%s] in %s",
                SHOW(def), SHOW(method));
        }
        if (fdef != nullptr) {
          auto fit = by_field.find(fdef);
          if (fit != by_field.end()) {
            cand = fit->second;
          } else {
            stats.field_unknown++;
          }
        }
        if (cand == nullptr || (info != nullptr && info != cand)) {
          if (cand != nullptr) {
            stats.conflicting_defs++;
          }
          stats.skipped_unresolved++;
          return nullptr;
        }
        info = cand;
      }
      return info;
    };

    auto holder_is_non_null = [&](IRInstruction* insn,
                                  bool ti_not_null) -> bool {
      if (ti_not_null) {
        return true;
      }
      if (receiver_insn == nullptr) {
        return false;
      }
      auto it = use_defs.find(live_range::Use{insn, 1});
      if (it == use_defs.end() || it->second.empty()) {
        return false;
      }
      for (auto* def : it->second) {
        if (def != receiver_insn) {
          return false;
        }
      }
      return true;
    };

    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto* insn = mie.insn;
      if (!opcode::is_invoke_virtual(insn->opcode())) {
        continue;
      }
      auto kind_it = updater_kinds.find(insn->get_method()->get_class());
      if (kind_it == updater_kinds.end()) {
        continue;
      }
      const Kind kind = kind_it->second;
      const auto* name = insn->get_method()->get_name();

      // Every argument after the holder must be a value of the flavor's type.
      // The functional-style operations -- getAndUpdate, updateAndGet,
      // getAndAccumulate, accumulateAndGet -- take a UnaryOperator or
      // BinaryOperator there instead, and compute the written value at runtime.
      // Their valueCheck cannot be discharged statically, so they are not
      // modeled at all rather than being judged against a value obligation
      // that does not describe them.
      //
      // Arity is checked alongside the name, not implied by it. The allow-list
      // says what the API calls an operation; it does not say that *this*
      // invoke has the API's signature. A method named `get` taking no holder
      // is not `get(T)`, and reading a holder out of it would index a source
      // that is not there.
      if (!atomic_field_updaters::is_modeled_operation(name->str()) ||
          insn->srcs_size() < 2 ||
          !writes_only_values(insn->get_method(), kind)) {
        stats.blocked_unmodeled_op++;
        continue;
      }

      const UpdaterInfo* info = resolve_updater(insn);
      if (info == nullptr) {
        continue;
      }
      auto eit = envs.find(insn);
      if (eit == envs.end()) {
        stats.skipped_unproven++;
        continue;
      }
      const auto& env = eit->second;

      // Guaranteed by the arity check in the gate above. Stated again here
      // because that guarantee sits twenty lines from the use depending on it,
      // and because reading a holder out of an invoke that has none is the
      // specific way this analysis has gone wrong before. `IRInstruction::src`
      // asserts in release regardless, so this costs nothing.
      redex_assert(insn->srcs_size() >= 2);
      auto holder_dom = env.get_type_domain(insn->src(1));
      auto holder_type = holder_dom.get_dex_type();
      if (!holder_type || !type::check_cast(*holder_type, info->holder)) {
        stats.blocked_holder_type++;
        stats.skipped_unproven++;
        continue;
      }
      const bool holder_null_proven =
          holder_is_non_null(insn, holder_dom.is_not_null());

      // Only the reference flavor performs a valueCheck; an int or long value
      // needs no type test. Among the modeled operations the written value is
      // always the last argument -- `compareAndSet(obj, expect, update)`
      // valueChecks `update` only -- and the check above has already excluded
      // the operations whose last argument is not a value at all.
      if (atomic_field_updaters::has_value_check(kind) &&
          insn->srcs_size() > 2) {
        reg_t value_reg = insn->src(insn->srcs_size() - 1);
        auto value_dom = env.get_type_domain(value_reg);
        auto value_type = value_dom.get_dex_type();
        // An Object-typed field admits every reference: `valueCheck` is
        // `v != null && !vclass.isInstance(v)`, and `Object.isInstance` is true
        // of anything non-null. Worth special-casing because `check_cast` needs
        // a loaded DexClass to walk the hierarchy and returns false without
        // one -- so a value of any framework type, `String` included, would
        // otherwise be blocked.
        bool ok = value_dom.is_null() ||
                  info->field_type == type::java_lang_Object() ||
                  (value_type && info->field_type &&
                   type::check_cast(*value_type, info->field_type));
        if (!ok) {
          stats.blocked_value_type++;
          stats.skipped_unproven++;
          continue;
        }
      }

      stats.feasible[{insn->get_method(), holder_null_proven}]++;
    }
    return stats;
  });

  mgr.set_metric("calls_skipped_unresolved_updater", totals.skipped_unresolved);
  mgr.set_metric("calls_skipped_unproven_types", totals.skipped_unproven);

  // Why a site was blocked, and why resolution failed: traced rather than
  // reported. These answer a question only someone investigating coverage
  // asks, and the totals they roll up into are already metrics.
  TRACE(ATOMUP, 2, "blocked: holder_type=%zu value_type=%zu unmodeled_op=%zu",
        totals.blocked_holder_type, totals.blocked_value_type,
        totals.blocked_unmodeled_op);
  TRACE(ATOMUP, 2,
        "unresolved: no_defs=%zu def_not_sget=%zu "
        "field_not_def=%zu "
        "field_unknown=%zu conflicting_defs=%zu",
        totals.no_defs, totals.def_not_sget, totals.field_not_def,
        totals.field_unknown, totals.conflicting_defs);

  size_t feasible_total = 0;
  size_t needs_null_check_total = 0;
  for (const auto& [key, n] : totals.feasible) {
    (key.second ? feasible_total : needs_null_check_total) += n;
  }

  // Everything below is for the trace and nothing else -- the per-(flavor,
  // operation) rows are not metrics, for the same reason the census breakdown
  // is not: which rows exist depends on the program. Guarded so the labels are
  // not formatted when no one is reading them.
  if (traceEnabled(ATOMUP, 2)) {
    std::vector<std::pair<std::string, size_t>> breakdown;
    breakdown.reserve(totals.feasible.size());
    for (const auto& [key, n] : totals.feasible) {
      const auto* mref = key.first;
      auto kit = updater_kinds.find(mref->get_class());
      breakdown.emplace_back(
          std::string(key.second ? "feasible_" : "needs_null_check_") +
              kind_name(kit->second) + "_" + mref->get_name()->str_copy() +
              show(mref->get_proto()),
          n);
    }
    std::sort(breakdown.begin(), breakdown.end());
    for (const auto& [label, n] : breakdown) {
      TRACE(ATOMUP, 2, "%s = %zu", label.c_str(), n);
    }
  }
  mgr.set_metric("feasible_total", feasible_total);
  mgr.set_metric("needs_null_check_total", needs_null_check_total);
  // What a full lowering would reach: proven sites plus those a null check
  // makes safe.
  mgr.set_metric("rewritable_total", feasible_total + needs_null_check_total);
  TRACE(ATOMUP, 1,
        "SUMMARY rewritable=%zu (proven=%zu needs_null_check=%zu) "
        "unresolved=%zu blocked_holder_type=%zu "
        "blocked_value_type=%zu "
        "unmodeled_op=%zu",
        feasible_total + needs_null_check_total, feasible_total,
        needs_null_check_total, totals.skipped_unresolved,
        totals.blocked_holder_type, totals.blocked_value_type,
        totals.blocked_unmodeled_op);
}

} // namespace

void AtomicFieldUpdaterLoweringPass::run_pass(DexStoresVector& stores,
                                              ConfigFiles& /* conf */,
                                              PassManager& mgr) {
  auto scope = build_class_scope(stores);
  census_ops(scope, mgr);

  // Recognition reads each holder's <clinit>, so it only needs the root store,
  // where the updaters this pass can act on are declared.
  Scope root_scope;
  redex_assert(!stores.empty());
  for (auto& dex : stores.at(0).get_dexen()) {
    for (auto* cls : dex) {
      root_scope.push_back(cls);
    }
  }

  KindCounts per_kind{};
  auto updaters = find_updaters(root_scope, per_kind);
  // Report every flavor, including any absent from the program: a metric that
  // vanishes at zero is indistinguishable from the pass not running.
  for (auto kind : atomic_field_updaters::all_kinds()) {
    mgr.set_metric(std::string("updaters_recognized_") + kind_name(kind),
                   per_kind.at(static_cast<size_t>(kind)));
  }
  mgr.set_metric("updaters_recognized", updaters.size());

  if (updaters.empty()) {
    return;
  }
  UnorderedMap<DexField*, const UpdaterInfo*> by_field;
  for (const auto& info : updaters) {
    by_field.emplace(info.updater, &info);
  }
  analyze_calls(scope, by_field, mgr);
}

static AtomicFieldUpdaterLoweringPass s_pass;
