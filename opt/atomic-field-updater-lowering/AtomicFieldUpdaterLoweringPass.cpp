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
  size_t total = 0;
  for (const auto& [name, n] : counts) {
    // The per-operation breakdown is traced, not reported as a metric: which
    // keys exist depends on which operations the program happens to call, so
    // the set of rows would differ between two builds being compared. Metrics
    // are for counters whose cardinality is fixed.
    TRACE(ATOMUP, 2, "%s = %zu", name.c_str(), n);
    total += n;
  }
  mgr.set_metric("ops_total", total);
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
}

static AtomicFieldUpdaterLoweringPass s_pass;
