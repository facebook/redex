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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CFGMutation.h"
#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "InitClassesWithSideEffects.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "ReflectionAnalysis.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeInference.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {
using atomic_field_updaters::Kind;
using atomic_field_updaters::kind_name;
using atomic_field_updaters::SYNTH_HOLDER_DESC;
using atomic_field_updaters::UNSAFE_DESC;

// A recognized updater: a `static final Atomic*FieldUpdater` field whose sole
// write is `newUpdater(holder, [T,] "field_name")` in the holder's <clinit>,
// and where `holder.field_name` is a non-static volatile field.
struct UpdaterInfo {
  DexField* updater; // the static final updater field
  DexType* holder; // class declaring the volatile field
  const DexString* field_name; // the volatile field's name
  const DexType* field_type{nullptr}; // the volatile field's declared type
  DexField* offset_field{nullptr}; // synthesized: static final long offset
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
                                   target->get_type(), nullptr, kind});
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

// The members synthesized for lowering: the shared `Unsafe` instance every
// rewritten call site loads from.
struct Helpers {
  DexField* s_unsafe{nullptr};
};

// The JDK symbols the synthesized `<clinit>`s reach for. Interned in one place
// rather than respelled at each use: two spellings of the same descriptor that
// drift apart produce two distinct refs and a dex that fails verification only
// at runtime.
//
// `java.lang.reflect.Field` and `sun.misc.Unsafe` deliberately stay here rather
// than joining `WellKnownTypes`: membership there opts a type into
// `IRTypeChecker`'s assignability checking, which is skipped for external types
// outside the set, and neither is a type this pass wants to claim that for.
DexType* unsafe_type() { return DexType::make_type(UNSAFE_DESC); }

DexType* reflect_field_type() {
  return DexType::make_type("Ljava/lang/reflect/Field;");
}

// Class.getDeclaredField(String) -> Field
DexMethodRef* class_get_declared_field() {
  return DexMethod::make_method(
      type::java_lang_Class(), DexString::make_string("getDeclaredField"),
      DexProto::make_proto(
          reflect_field_type(),
          DexTypeList::make_type_list({type::java_lang_String()})));
}

// Field.setAccessible(boolean) -> void
DexMethodRef* field_set_accessible() {
  return DexMethod::make_method(
      reflect_field_type(), DexString::make_string("setAccessible"),
      DexProto::make_proto(type::_void(),
                           DexTypeList::make_type_list({type::_boolean()})));
}

// Field.get(Object) -> Object
DexMethodRef* field_get_value() {
  return DexMethod::make_method(
      reflect_field_type(), DexString::make_string("get"),
      DexProto::make_proto(
          type::java_lang_Object(),
          DexTypeList::make_type_list({type::java_lang_Object()})));
}

// Unsafe.objectFieldOffset(Field) -> long
DexMethodRef* unsafe_object_field_offset() {
  return DexMethod::make_method(
      unsafe_type(), DexString::make_string("objectFieldOffset"),
      DexProto::make_proto(
          type::_long(), DexTypeList::make_type_list({reflect_field_type()})));
}

// Synthesizes the class holding the shared `Unsafe` instance,
// obtained reflectively in its <clinit>. The per-field offsets deliberately do
// not live here -- see add_offsets_to_holders.
Helpers synthesize_unsafe_holder(DexStoresVector& stores) {
  auto* synth_type = DexType::make_type(SYNTH_HOLDER_DESC);
  auto* object_type = type::java_lang_Object();

  ClassCreator cc(synth_type);
  cc.set_super(object_type);
  cc.set_access(ACC_PUBLIC | ACC_FINAL);

  auto* s_unsafe =
      DexField::make_field(synth_type, DexString::make_string("sUnsafe"),
                           unsafe_type())
          ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
  // Synthesized members start with an empty deobfuscated name; once they
  // survive to the end (i.e. once call sites actually use them) the final
  // duplicate-deobfuscated-name check rejects the second one.
  s_unsafe->set_deobfuscated_name(show(s_unsafe));
  s_unsafe->rstate.set_generated();
  cc.add_field(s_unsafe);

  auto* get_declared_field = class_get_declared_field();
  auto* set_accessible = field_set_accessible();
  auto* field_get = field_get_value();

  MethodCreator mc(
      synth_type, DexString::make_string("<clinit>"),
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({})),
      ACC_STATIC | ACC_CONSTRUCTOR);
  auto* mb = mc.get_main_block();

  // sUnsafe = (Unsafe) Unsafe.class.getDeclaredField("theUnsafe").get(null);
  // with setAccessible(true) in between.
  auto cls_loc = mc.make_local(type::java_lang_Class());
  mb->load_const(cls_loc, unsafe_type());
  auto name_loc = mc.make_local(type::java_lang_String());
  mb->load_const(name_loc, DexString::make_string("theUnsafe"));
  auto field_loc = mc.make_local(reflect_field_type());
  mb->invoke(OPCODE_INVOKE_VIRTUAL, get_declared_field, {cls_loc, name_loc});
  mb->move_result(field_loc, reflect_field_type());
  auto true_loc = mc.make_local(type::_boolean());
  mb->load_const(true_loc, (int32_t)1);
  mb->invoke(OPCODE_INVOKE_VIRTUAL, set_accessible, {field_loc, true_loc});
  auto null_loc = mc.make_local(object_type);
  mb->load_null(null_loc);
  auto obj_loc = mc.make_local(object_type);
  mb->invoke(OPCODE_INVOKE_VIRTUAL, field_get, {field_loc, null_loc});
  mb->move_result(obj_loc, object_type);
  mb->check_cast(obj_loc, unsafe_type());
  mb->sput(s_unsafe, obj_loc);

  mb->ret_void();

  auto* clinit = mc.create();
  clinit->set_deobfuscated_name(show(clinit));
  clinit->rstate.set_generated();
  cc.add_method(clinit);

  auto* cls = cc.create();
  cls->set_deobfuscated_name(show(cls));
  // Nothing here has a source counterpart: no stable name to preserve, and no
  // place in coldstart tracking. Saying so keeps it out of class merging's
  // models and out of original-name recording.
  cls->rstate.set_generated();
  // cfg-friendly contract: every method with code must leave with a built CFG.
  clinit->get_code()->build_cfg();

  redex_assert(!stores.empty());
  auto& dexen = stores.at(0).get_dexen();
  redex_assert(!dexen.empty());
  dexen.at(0).push_back(cls);
  return Helpers{s_unsafe};
}

// Give each recognized updater a `static final long` offset *in its own holder
// class*, initialized at the top of that class's own <clinit>.
//
// The offset cannot live in a shared synthetic class. Computing it needs
// `Holder.class.getDeclaredField(name)`, and a holder is frequently not public
// -- `kotlinx.coroutines` internals, and any nested class -- so a `const-class`
// from another package throws IllegalAccessError when the shared class
// initializes, taking every later use down with NoClassDefFoundError. Emitting
// the lookup inside the holder sidesteps access control entirely: a class may
// always reflect over itself.
//
// It is also better placed. The work is exactly what `newUpdater` already did,
// so it now happens when that class initializes, rather than dragging every
// holder in the program into the initializer of whichever one ran first.
void add_offsets_to_holders(std::vector<UpdaterInfo>& updaters,
                            DexField* s_unsafe,
                            PassManager& mgr) {
  auto* long_type = type::_long();
  auto* get_declared_field = class_get_declared_field();
  auto* object_field_offset = unsafe_object_field_offset();

  size_t added = 0;
  for (auto& info : updaters) {
    auto* holder_cls = type_class(info.holder);
    if (holder_cls == nullptr) {
      continue;
    }
    auto* clinit = holder_cls->get_clinit();
    // Recognition required the updater to be initialized in the holder's
    // <clinit>, so one exists with code.
    if (clinit == nullptr || clinit->get_code() == nullptr) {
      continue;
    }

    // Prepend, rather than append: the computation depends on nothing else in
    // the <clinit>, and the entry block is the one point guaranteed to run
    // before any other statement regardless of the method's control flow.
    auto& cfg = clinit->get_code()->cfg();

    // Anchor on the first real instruction: a block can open with a position
    // or source-block entry, which is not a valid instruction iterator.
    //
    // Settled before the field is created, so that the field and its
    // initializer stand or fall together. A field added without one would read
    // 0, and lowering -- which only checks that the field exists -- would then
    // address the object header instead of the intended field.
    auto* entry = cfg.entry_block();
    auto first = entry->get_first_insn();
    if (first == entry->end()) {
      continue;
    }

    auto* offset = DexField::make_field(
                       info.holder,
                       DexString::make_string(info.updater->str() + "$offset"),
                       long_type)
                       ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    offset->set_deobfuscated_name(show(offset));
    offset->rstate.set_generated();
    holder_cls->add_field(offset);
    info.offset_field = offset;

    reg_t u_reg = cfg.allocate_temp();
    reg_t cls_reg = cfg.allocate_temp();
    reg_t name_reg = cfg.allocate_temp();
    reg_t field_reg = cfg.allocate_temp();
    reg_t off_reg = cfg.allocate_wide_temp();

    std::vector<IRInstruction*> init;
    auto* load_unsafe = new IRInstruction(OPCODE_SGET_OBJECT);
    load_unsafe->set_field(s_unsafe);
    auto* load_unsafe_res =
        new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
    load_unsafe_res->set_dest(u_reg);
    init.push_back(load_unsafe);
    init.push_back(load_unsafe_res);

    auto* const_cls = new IRInstruction(OPCODE_CONST_CLASS);
    const_cls->set_type(info.holder);
    auto* const_cls_res = new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
    const_cls_res->set_dest(cls_reg);
    init.push_back(const_cls);
    init.push_back(const_cls_res);

    auto* const_str = new IRInstruction(OPCODE_CONST_STRING);
    const_str->set_string(info.field_name);
    auto* const_str_res = new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
    const_str_res->set_dest(name_reg);
    init.push_back(const_str);
    init.push_back(const_str_res);

    auto* get_field = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
    get_field->set_method(get_declared_field);
    get_field->set_srcs_size(2);
    get_field->set_src(0, cls_reg);
    get_field->set_src(1, name_reg);
    auto* get_field_res = new IRInstruction(OPCODE_MOVE_RESULT_OBJECT);
    get_field_res->set_dest(field_reg);
    init.push_back(get_field);
    init.push_back(get_field_res);

    auto* to_offset = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
    to_offset->set_method(object_field_offset);
    to_offset->set_srcs_size(2);
    to_offset->set_src(0, u_reg);
    to_offset->set_src(1, field_reg);
    auto* to_offset_res = new IRInstruction(OPCODE_MOVE_RESULT_WIDE);
    to_offset_res->set_dest(off_reg);
    init.push_back(to_offset);
    init.push_back(to_offset_res);

    auto* store = new IRInstruction(OPCODE_SPUT_WIDE);
    store->set_field(offset);
    store->set_src(0, off_reg);
    init.push_back(store);

    cfg::CFGMutation mutation(cfg);
    mutation.insert_before(entry->to_cfg_instruction_iterator(first), init);
    mutation.flush();
    added++;
  }
  mgr.set_metric("offset_fields_added", added);
}

// Kotlin emits the `$FU` field as private and reads it through a
// synthetic static getter (`get_state$volatile$FU()`), with an
// `access$getOwner$volatile$FU()` bridge on top for nested-class use. So at a
// call site the updater receiver is defined by an invoke-static, not by a bare
// sget-object.
//
// Those calls are found by following the receiver rather than by searching for
// them: the use-def walk that resolution already performs lands on the accessor
// by construction, so there is no shape to guess at and nothing to decide about
// which methods are worth considering.

// Does this method invoke an updater operation at all? Asked before any
// dataflow machinery is built, because the great majority of methods touch no
// updater and `MoveAwareChains` is not free.
bool uses_updater(cfg::ControlFlowGraph& cfg,
                  const UnorderedMap<const DexType*, Kind>& updater_kinds) {
  for (auto& mie : cfg::InstructionIterable(cfg)) {
    auto* insn = mie.insn;
    if (opcode::is_invoke_virtual(insn->opcode()) &&
        updater_kinds.count(insn->get_method()->get_class()) != 0u) {
      return true;
    }
  }
  return false;
}

// Is `m` an accessor for a recognized updater? On success `chain` gets every
// method on the path, which is what has to be inlined: flattening a bridge
// alone would leave the getter it calls still standing between the call site
// and the field.
//
// The pattern is small and fixed. A method reads one recognized updater field
// and hands it back, or hands back what one other such method returns. A
// second read, a second call, or any other instruction is not this pattern, so
// bail and leave the call site unresolved. Having exactly one source is also
// what makes the chain a simple path, walked with a loop.
//
// Every step stays on the accessor's own class -- the field it reads and the
// method it delegates to are both declared there. That is not a coincidence to
// be exploited but the JVM's rule for synthetic accessors, which are emitted in
// the class owning the private member they reach. Requiring it is what makes
// the body free of observable effect in the strict sense: the only <clinit> a
// permitted instruction can trigger is the accessor's own class's, and calling
// the accessor already triggered that.
//
// Two properties fall out of that whitelist rather than being tested for.
// Nothing else in it can produce an object value, so whatever `return-object`
// hands back must be the field that was read or the result of the delegate --
// there is no second value for it to return. And a method taking arguments has
// `load-param` instructions, which the whitelist does not admit, so it bails;
// being static is guaranteed by arriving here from an `invoke-static`.
//
// Not a safety condition -- inlining preserves semantics whatever the body
// does, class initialization included -- but scope discipline: "log, then
// return NEXT" would be copied into every caller for no reason.
bool is_accessor_chain(DexMethod* m,
                       const UnorderedSet<DexField*>& recognized_fields,
                       UnorderedSet<DexMethod*>* chain) {
  UnorderedSet<DexMethod*> walked;
  while (m != nullptr && m->get_code() != nullptr &&
         !m->rstate.no_optimizations() && walked.insert(m).second) {
    DexField* field = nullptr;
    DexMethod* delegate = nullptr;
    for (auto& mie : cfg::InstructionIterable(m->get_code()->cfg())) {
      auto* insn = mie.insn;
      auto op = insn->opcode();
      if (opcode::is_sget_object(op) && field == nullptr &&
          delegate == nullptr && insn->get_field()->is_def()) {
        field = insn->get_field()->as_def();
      } else if (opcode::is_invoke_static(op) && field == nullptr &&
                 delegate == nullptr) {
        delegate = resolve_method(insn->get_method(), MethodSearch::Static);
      } else if (!opcode::is_move_result_pseudo_object(op) &&
                 !opcode::is_move_result_object(op) &&
                 !opcode::is_move_object(op) && !opcode::is_return_object(op)) {
        return false;
      }
    }
    if (delegate != nullptr) {
      if (delegate->get_class() != m->get_class()) {
        return false;
      }
      m = delegate;
      continue;
    }
    if (field == nullptr || field->get_class() != m->get_class() ||
        !recognized_fields.contains(field)) {
      return false;
    }
    insert_unordered_iterable(*chain, walked);
    return true;
  }
  return false;
}

// The accessors standing between an operation and its updater field.
//
// Driven by the call sites rather than by a scan of the program: every
// operation's receiver is walked back through `MoveAwareChains`, and a receiver
// defined by an invoke-static names the accessor directly. So relevance is not
// judged, it is where the walk arrived, and a bridge is reached by following
// the chain rather than by iterating selection to a fixed point.
UnorderedSet<DexMethod*> find_receiver_chain_accessors(
    const Scope& scope,
    const UnorderedMap<const DexType*, Kind>& updater_kinds,
    const UnorderedSet<DexField*>& recognized_fields,
    PassManager& mgr) {
  InsertOnlyConcurrentSet<DexMethod*> selected;
  // Callees a receiver came from that could not be followed. Recorded per
  // method rather than per call site, so a body reached from twenty sites
  // counts once.
  InsertOnlyConcurrentSet<DexMethod*> rejected;

  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto* code = method->get_code();
    if (code == nullptr || method->rstate.no_optimizations()) {
      return;
    }
    always_assert(code->cfg_built());
    auto& cfg = code->cfg();
    if (!uses_updater(cfg, updater_kinds)) {
      return;
    }
    live_range::MoveAwareChains chains(cfg);
    auto use_defs = chains.get_use_def_chains();

    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto* insn = mie.insn;
      if (!opcode::is_invoke_virtual(insn->opcode()) ||
          updater_kinds.count(insn->get_method()->get_class()) == 0u) {
        continue;
      }
      auto it = use_defs.find(live_range::Use{insn, 0});
      if (it == use_defs.end()) {
        continue;
      }
      for (auto* def : it->second) {
        // An sget-object receiver already resolves; anything that is not a
        // call is not something inlining could help with.
        if (!opcode::is_invoke_static(def->opcode())) {
          continue;
        }
        auto* callee = resolve_method(def->get_method(), MethodSearch::Static);
        UnorderedSet<DexMethod*> chain;
        if (is_accessor_chain(callee, recognized_fields, &chain)) {
          for (auto* on_chain : UnorderedIterable(chain)) {
            selected.insert(on_chain);
          }
        } else if (callee != nullptr) {
          rejected.insert(callee);
          TRACE(ATOMUP, 3, "receiver of %s comes from %s, which is not a chain",
                SHOW(insn), SHOW(callee));
        }
      }
    }
  });

  UnorderedSet<DexMethod*> result;
  insert_unordered_iterable(result, selected);
  mgr.set_metric("accessors_rejected_impure", rejected.size());
  return result;
}

// Inline the selected accessors so the updater reaches its call sites as a
// plain sget-object. Uses the shared inliner rather than splicing by hand: it
// already models init-class side effects, which matters because a bridge in one
// class delegating to a getter in another would otherwise silently drop the
// bridge class's static initializer.
size_t inline_updater_accessors(DexStoresVector& stores,
                                const Scope& scope,
                                ConfigFiles& conf,
                                PassManager& mgr,
                                const UnorderedSet<DexMethod*>& candidates) {
  mgr.set_metric("accessors_selected", candidates.size());
  if (candidates.empty()) {
    // Record it anyway: a metric that vanishes when the count is zero is
    // indistinguishable from the pass not having run.
    mgr.set_metric("accessors_inlined", 0);
    return 0;
  }
  auto method_override_graph = method_override_graph::build_graph(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns(), method_override_graph.get());
  ConcurrentMethodResolverDeprecated concurrent_method_resolver;
  auto inliner_config =
      *conf.get_global_config().get_config_by_name<InlinerConfig>("inliner");
  // The global config has `shrink_other_methods` on, which schedules *every*
  // method in the scope for shrinking, not just the ones inlined into. That is
  // right for MethodInlinePass and wrong here: it would run a whole-app shrink
  // as a side effect of inlining 65 accessors, duplicating ShrinkerPass a few
  // passes ahead of it, and leave this pass's size effect unmeasurable because
  // the delta would be dominated by work that has nothing to do with atomics.
  // Callers we inline into are still shrunk.
  inliner_config.shrink_other_methods = false;
  MultiMethodInliner inliner(
      scope, init_classes_with_side_effects, stores, conf, candidates,
      std::ref(concurrent_method_resolver), inliner_config,
      mgr.get_redex_options().min_sdk, MultiMethodInlinerMode::InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined().size();
  mgr.set_metric("accessors_inlined", inlined);
  TRACE(ATOMUP, 1,
        "SUMMARY accessors selected=%zu inlined=%zu (rejected_impure counted "
        "separately)",
        candidates.size(), inlined);
  return inlined;
}

// Which updater operation a call site invokes. Only the reference flavor's
// three basic operations are lowered; other flavors and operations are counted
// and left in place.
enum class Op { GET, SET, COMPARE_AND_SET };

// The sun.misc.Unsafe operation a given updater operation lowers to.
DexMethodRef* unsafe_op_ref(Op op) {
  auto* object_type = type::java_lang_Object();
  auto* long_type = type::_long();
  auto* bool_type = type::_boolean();
  switch (op) {
  case Op::GET:
    return DexMethod::make_method(
        unsafe_type(), DexString::make_string("getObjectVolatile"),
        DexProto::make_proto(object_type, DexTypeList::make_type_list(
                                              {object_type, long_type})));
  case Op::SET:
    return DexMethod::make_method(
        unsafe_type(), DexString::make_string("putObjectVolatile"),
        DexProto::make_proto(type::_void(),
                             DexTypeList::make_type_list(
                                 {object_type, long_type, object_type})));
  case Op::COMPARE_AND_SET:
    return DexMethod::make_method(
        unsafe_type(), DexString::make_string("compareAndSwapObject"),
        DexProto::make_proto(
            bool_type,
            DexTypeList::make_type_list(
                {object_type, long_type, object_type, object_type})));
  }
  not_reached();
}

// Build a static-field load (sget + pseudo) into `dst`.
void emit_sget(std::vector<IRInstruction*>* out,
               IROpcode sget_op,
               IROpcode pseudo_op,
               DexField* field,
               reg_t dst) {
  auto* g = new IRInstruction(sget_op);
  g->set_field(field);
  auto* p = new IRInstruction(pseudo_op);
  p->set_dest(dst);
  out->push_back(g);
  out->push_back(p);
}

// Every updater operation call site is measured against the obligations that
// lowering it to `sun.misc.Unsafe` must discharge. Sites that discharge all of
// them are rewritten; the rest are counted and left alone.
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

// A site the analysis found lowerable, and what emission needs to rewrite it.
// Recorded by the parallel phase, consumed by the serial one; nothing here
// refers to a CFG iterator, which would not outlive the walk.
struct Rewrite {
  IRInstruction* insn;
  const UpdaterInfo* info;
  Op op;
};

// Filled from many threads, read from one. Keyed by method, so the serial
// phase can visit methods in scope order rather than in completion order.
using RewritePlan = InsertOnlyConcurrentMap<DexMethod*, std::vector<Rewrite>>;

// Counted per thread and summed after the walk. The analysis visits every
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

// What judging a call site needs from the method containing it, built once per
// method and queried at each site.
struct MethodAnalysis {
  DexMethod* method;
  const UnorderedMap<DexField*, const UpdaterInfo*>& by_field;
  const UnorderedMap<const IRInstruction*, type_inference::TypeEnvironment>&
      envs;
  const live_range::UseDefChains& use_defs;
  // The load-param defining `this`, or null in a static method. An instance
  // method's receiver is non-null on entry -- the invoke that got us here would
  // already have thrown otherwise -- but TypeInference does not encode that.
  IRInstruction* receiver_insn;
};

// The updater behind a call's receiver, or null if the receiver does not
// resolve to exactly one recognized updater.
const UpdaterInfo* resolve_updater(IRInstruction* insn,
                                   const MethodAnalysis& ma,
                                   Stats* stats) {
  auto it = ma.use_defs.find(live_range::Use{insn, 0});
  if (it == ma.use_defs.end() || it->second.empty()) {
    stats->skipped_unresolved++;
    stats->no_defs++;
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
        stats->field_not_def++;
      }
    } else {
      stats->def_not_sget++;
      TRACE(ATOMUP, 4, "unresolved (def not sget-object): def=[%s] in %s",
            SHOW(def), SHOW(ma.method));
    }
    if (fdef != nullptr) {
      auto fit = ma.by_field.find(fdef);
      if (fit != ma.by_field.end()) {
        cand = fit->second;
      } else {
        stats->field_unknown++;
      }
    }
    if (cand == nullptr || (info != nullptr && info != cand)) {
      if (cand != nullptr) {
        stats->conflicting_defs++;
      }
      stats->skipped_unresolved++;
      return nullptr;
    }
    info = cand;
  }
  return info;
}

bool holder_is_non_null(IRInstruction* insn,
                        bool ti_not_null,
                        const MethodAnalysis& ma) {
  if (ti_not_null) {
    return true;
  }
  if (ma.receiver_insn == nullptr) {
    return false;
  }
  auto it = ma.use_defs.find(live_range::Use{insn, 1});
  if (it == ma.use_defs.end() || it->second.empty()) {
    return false;
  }
  for (auto* def : it->second) {
    if (def != ma.receiver_insn) {
      return false;
    }
  }
  return true;
}

// Judges one updater call against the obligations `Atomic*FieldUpdater`
// discharges at runtime, recording why it was turned away. Returns nothing for
// a site that is not rewritten -- which includes feasible sites whose operation
// this pass cannot yet express.
std::optional<Rewrite> classify_site(IRInstruction* insn,
                                     Kind kind,
                                     const MethodAnalysis& ma,
                                     Stats* stats) {
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
      insn->srcs_size() < 2 || !writes_only_values(insn->get_method(), kind)) {
    stats->blocked_unmodeled_op++;
    return std::nullopt;
  }

  // Only the reference flavor's get/set/compareAndSet are lowered so far.
  // Every site is still judged against the obligations below, so the metrics
  // report the full reachable opportunity rather than only what is acted on.
  Op op = Op::GET;
  bool emittable = kind == Kind::REFERENCE;
  if (name->str() == "get") {
    op = Op::GET;
  } else if (name->str() == "set") {
    op = Op::SET;
  } else if (name->str() == "compareAndSet") {
    op = Op::COMPARE_AND_SET;
  } else {
    emittable = false;
  }

  const UpdaterInfo* info = resolve_updater(insn, ma, stats);
  if (info == nullptr) {
    return std::nullopt;
  }
  auto eit = ma.envs.find(insn);
  if (eit == ma.envs.end()) {
    stats->skipped_unproven++;
    return std::nullopt;
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
    stats->blocked_holder_type++;
    stats->skipped_unproven++;
    return std::nullopt;
  }
  const bool holder_null_proven =
      holder_is_non_null(insn, holder_dom.is_not_null(), ma);

  // Only the reference flavor performs a valueCheck; an int or long value
  // needs no type test. Among the modeled operations the written value is
  // always the last argument -- `compareAndSet(obj, expect, update)`
  // valueChecks `update` only -- and the check above has already excluded
  // the operations whose last argument is not a value at all.
  if (atomic_field_updaters::has_value_check(kind) && insn->srcs_size() > 2) {
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
              (value_type.has_value() && info->field_type != nullptr &&
               type::check_cast(*value_type, info->field_type));
    if (!ok) {
      stats->blocked_value_type++;
      stats->skipped_unproven++;
      return std::nullopt;
    }
  }

  stats->feasible[{insn->get_method(), holder_null_proven}]++;

  // A holder reachable only behind a null check is counted above but not
  // emitted: without the check, `Unsafe` on a null holder is a raw memory
  // access rather than the ClassCastException `accessCheck` would throw.
  if (!emittable || !holder_null_proven) {
    return std::nullopt;
  }
  return Rewrite{insn, info, op};
}

// Reads the whole program for lowerable sites and records them in `rewrites`.
// Nothing is mutated here: emission is a separate serial phase, so the output
// does not depend on which thread reached a method first.
Stats analyze_calls(const Scope& scope,
                    const UnorderedMap<DexField*, const UpdaterInfo*>& by_field,
                    const UnorderedMap<const DexType*, Kind>& updater_kinds,
                    RewritePlan* rewrites) {
  return walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
    Stats stats;
    auto* code = method->get_code();
    if (code == nullptr || method->rstate.no_optimizations()) {
      return stats;
    }
    always_assert(code->cfg_built());
    auto& cfg = code->cfg();
    if (!uses_updater(cfg, updater_kinds)) {
      return stats;
    }

    type_inference::TypeInference ti(cfg);
    ti.run(method);

    // Move-aware use-def chains resolve the receiver and the holder across
    // basic blocks: a load hoisted out of a loop lands in a different block
    // from the call that uses it.
    live_range::MoveAwareChains chains(cfg);
    auto use_defs = chains.get_use_def_chains();

    IRInstruction* receiver_insn = nullptr;
    if (!is_static(method)) {
      auto params = cfg.get_param_instructions();
      auto first = params.begin();
      if (first != params.end() &&
          opcode::is_load_param_object(first->insn->opcode())) {
        receiver_insn = first->insn;
      }
    }
    MethodAnalysis ma{method, by_field, ti.get_type_environments(), use_defs,
                      receiver_insn};

    std::vector<Rewrite> planned;
    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto* insn = mie.insn;
      if (!opcode::is_invoke_virtual(insn->opcode())) {
        continue;
      }
      auto kind_it = updater_kinds.find(insn->get_method()->get_class());
      if (kind_it == updater_kinds.end()) {
        continue;
      }
      auto rewrite = classify_site(insn, kind_it->second, ma, &stats);
      if (rewrite.has_value()) {
        planned.push_back(*rewrite);
      }
    }
    if (!planned.empty()) {
      rewrites->emplace(method, std::move(planned));
    }
    return stats;
  });
}

// The instructions replacing one updater call: load the shared `Unsafe` and the
// field's offset, then the unsafe operation.
std::vector<IRInstruction*> build_replacement(
    cfg::ControlFlowGraph& cfg,
    const cfg::InstructionIterator& it,
    const Rewrite& rewrite,
    const Helpers& helpers) {
  IRInstruction* insn = rewrite.insn;
  const Op op = rewrite.op;

  reg_t unsafe_reg = cfg.allocate_temp();
  reg_t offset_reg = cfg.allocate_wide_temp();
  std::vector<IRInstruction*> repl;
  emit_sget(&repl, OPCODE_SGET_OBJECT, IOPCODE_MOVE_RESULT_PSEUDO_OBJECT,
            helpers.s_unsafe, unsafe_reg);
  emit_sget(&repl, OPCODE_SGET_WIDE, IOPCODE_MOVE_RESULT_PSEUDO_WIDE,
            rewrite.info->offset_field, offset_reg);

  auto* inv = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  inv->set_method(unsafe_op_ref(op));
  switch (op) {
  case Op::GET:
    inv->set_srcs_size(3);
    break;
  case Op::SET:
    inv->set_srcs_size(4);
    inv->set_src(3, insn->src(2)); // new value
    break;
  case Op::COMPARE_AND_SET:
    inv->set_srcs_size(5);
    inv->set_src(3, insn->src(2)); // expected
    inv->set_src(4, insn->src(3)); // update
    break;
  }
  inv->set_src(0, unsafe_reg);
  inv->set_src(1, insn->src(1)); // holder
  inv->set_src(2, offset_reg);
  repl.push_back(inv);

  // CFGMutation::replace drops the replaced invoke's move-result, so
  // re-emit one unless the result was discarded.
  if (op != Op::SET) {
    auto mr_it = cfg.move_result_of(it);
    if (!mr_it.is_end()) {
      auto* mr = new IRInstruction(op == Op::GET ? OPCODE_MOVE_RESULT_OBJECT
                                                 : OPCODE_MOVE_RESULT);
      mr->set_dest(mr_it->insn->dest());
      repl.push_back(mr);
    }
  }
  return repl;
}

// What emission did, for the metrics.
struct EmitStats {
  size_t rewritten{0};
};

// Applies the plan. Single threaded and in scope order: emission is
// proportional to the sites that survived analysis -- a few hundred on a large
// app -- so serialising it costs nothing, and it keeps `CFGMutation`, register
// allocation and method ref interning off the parallel path entirely.
EmitStats emit_rewrites(const Scope& scope,
                        const RewritePlan& rewrites,
                        const std::function<const Helpers&()>& ensure_helpers) {
  EmitStats emitted;
  // Nothing is synthesized until here: with no site planned, the app is left
  // without the holder class rather than carrying an unreachable one.
  if (rewrites.empty()) {
    return emitted;
  }
  const Helpers& helpers = ensure_helpers();

  walk::methods(scope, [&](DexMethod* method) {
    const auto* planned = rewrites.get(method);
    if (planned == nullptr) {
      return;
    }
    UnorderedMap<const IRInstruction*, const Rewrite*> by_insn;
    for (const auto& r : *planned) {
      by_insn.emplace(r.insn, &r);
    }
    auto& cfg = method->get_code()->cfg();
    cfg::CFGMutation mutation(cfg);
    auto iterable = cfg::InstructionIterable(cfg);
    for (auto it = iterable.begin(); it != iterable.end(); ++it) {
      auto rit = by_insn.find(it->insn);
      if (rit == by_insn.end()) {
        continue;
      }
      const Rewrite& rewrite = *rit->second;
      // An updater whose holder could not be given an offset -- no class, or
      // a <clinit> with nowhere to insert -- cannot be rewritten. Recognition
      // is supposed to rule those out, but the offset is what the rewrite
      // reads, so this does not lean on that: substituting a null field here
      // would put an unresolved field reference into the output.
      if (rewrite.info->offset_field == nullptr) {
        continue;
      }
      mutation.replace(it, build_replacement(cfg, it, rewrite, helpers));
      emitted.rewritten++;
    }
    mutation.flush();
  });
  return emitted;
}

void report(PassManager& mgr,
            const Stats& totals,
            const EmitStats& emitted,
            const UnorderedMap<const DexType*, Kind>& updater_kinds) {
  mgr.set_metric("calls_rewritten", emitted.rewritten);
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
  TRACE(
      ATOMUP, 1,
      "SUMMARY rewritten=%zu rewritable=%zu (proven=%zu needs_null_check=%zu) "
      "unresolved=%zu blocked_holder_type=%zu blocked_value_type=%zu "
      "unmodeled_op=%zu",
      emitted.rewritten, feasible_total + needs_null_check_total,
      feasible_total, needs_null_check_total, totals.skipped_unresolved,
      totals.blocked_holder_type, totals.blocked_value_type,
      totals.blocked_unmodeled_op);
}

// `ensure_helpers` synthesizes the shared `Unsafe` holder and the per-field
// offsets, and is called only if the analysis finds a site to emit. An app with
// updaters it cannot lower should not be given an unreachable class whose
// <clinit> reflects over `sun.misc.Unsafe`.
void lower_calls(const Scope& scope,
                 const UnorderedMap<DexField*, const UpdaterInfo*>& by_field,
                 const std::function<const Helpers&()>& ensure_helpers,
                 PassManager& mgr) {
  const auto updater_kinds = atomic_field_updaters::present_kinds();
  if (updater_kinds.empty()) {
    return;
  }
  RewritePlan rewrites;
  const Stats totals = analyze_calls(scope, by_field, updater_kinds, &rewrites);
  const EmitStats emitted = emit_rewrites(scope, rewrites, ensure_helpers);
  report(mgr, totals, emitted, updater_kinds);
}

} // namespace

void AtomicFieldUpdaterLoweringPass::run_pass(DexStoresVector& stores,
                                              ConfigFiles& conf,
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
  // Flatten the synthetic accessors Kotlin puts between a call site
  // and a recognized updater field, so resolution below sees a plain field
  // read rather than a call.
  UnorderedSet<DexField*> recognized_fields;
  for (const auto& info : updaters) {
    recognized_fields.insert(info.updater);
  }
  const auto updater_kinds = atomic_field_updaters::present_kinds();
  auto accessors = find_receiver_chain_accessors(scope, updater_kinds,
                                                 recognized_fields, mgr);
  inline_updater_accessors(stores, scope, conf, mgr, accessors);

  // Deferred: synthesized on first use, from inside `lower_calls`, once the
  // analysis has found a site worth emitting.
  std::optional<Helpers> helpers;
  auto ensure_helpers = [&]() -> const Helpers& {
    if (!helpers) {
      helpers = synthesize_unsafe_holder(stores);
      add_offsets_to_holders(updaters, helpers->s_unsafe, mgr);
    }
    return *helpers;
  };
  lower_calls(scope, by_field, ensure_helpers, mgr);
}

static AtomicFieldUpdaterLoweringPass s_pass;
