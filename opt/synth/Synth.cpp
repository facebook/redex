/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Synth.h"

#include <signal.h>
#include <stdio.h>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ClassHierarchy.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Mutators.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "SynthConfig.h"
#include "Walkers.h"


constexpr const char* METRIC_GETTERS_REMOVED = "getter_methods_removed_count";
constexpr const char* METRIC_WRAPPERS_REMOVED = "wrapper_methods_removed_count";
constexpr const char* METRIC_CTORS_REMOVED = "constructors_removed_count";

namespace {
struct SynthMetrics {
  SynthMetrics()
      : getters_removed_count(0),
        wrappers_removed_count(0),
        ctors_removed_count(0) {}

  size_t getters_removed_count;
  size_t wrappers_removed_count;
  size_t ctors_removed_count;
};
} // anonymous namespace

bool is_static_synthetic(DexMethod* meth) {
  return is_static(meth) && is_synthetic(meth);
}

bool can_optimize(DexMethod* meth, const SynthConfig& synthConfig) {
  return !synthConfig.synth_only || is_static_synthetic(meth);
}

bool can_remove(DexMethod* meth, const SynthConfig& synthConfig) {
  return synthConfig.remove_pub || !is_public(meth);
}

/*
 * Matches the pattern:
 *   iget-TYPE vB, FIELD
 *   move-result-pseudo-object vA
 *   return-TYPE vA
 */
DexField* trivial_get_field_wrapper(DexMethod* m) {
  auto code = m->get_code();
  if (code == nullptr) return nullptr;

  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  auto end = ii.end();
  while (it != end && opcode::is_load_param(it->insn->opcode())) {
    ++it;
  }

  if (!is_iget(it->insn->opcode())) return nullptr;

  auto iget = it->insn;
  uint16_t iget_dest = ir_list::move_result_pseudo_of(it.unwrap())->dest();
  std::advance(it, 2);

  if (!is_return_value(it->insn->opcode())) return nullptr;

  uint16_t ret_reg = it->insn->src(0);
  if (ret_reg != iget_dest) return nullptr;
  ++it;

  if (it != end) return nullptr;

  // Check to make sure we have a concrete field reference.
  auto def = resolve_field(iget->get_field(), FieldSearch::Instance);
  if (def == nullptr) return nullptr;
  if (!def->is_concrete()) {
    return nullptr;
  }

  return def;
}

/*
 * Matches the pattern:
 *   sget-TYPE FIELD
 *   move-result-pseudo-object vA
 *   return-TYPE vA
 */
DexField* trivial_get_static_field_wrapper(DexMethod* m) {
  auto code = m->get_code();
  if (code == nullptr) return nullptr;

  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  auto end = ii.end();
  while (it != end && opcode::is_load_param(it->insn->opcode())) {
    ++it;
  }

  if (!is_sget(it->insn->opcode())) return nullptr;

  auto sget = it->insn;
  uint16_t sget_dest = ir_list::move_result_pseudo_of(it.unwrap())->dest();
  std::advance(it, 2);

  if (!is_return_value(it->insn->opcode())) return nullptr;

  uint16_t ret_reg = it->insn->src(0);
  if (ret_reg != sget_dest) return nullptr;
  ++it;

  if (it != end) return nullptr;

  // Check to make sure we have a concrete field reference.
  auto def = resolve_field(sget->get_field(), FieldSearch::Static);
  if (def == nullptr) return nullptr;
  if (!def->is_concrete()) {
    return nullptr;
  }

  return def;
}

/*
 * Matches the pattern:
 *   invoke-(direct|static) {vA, ..., vB} METHOD
 *   (  move-result-TYPE v0
 *      return-TYPE v0
 *    | return-void )
 */
DexMethod* trivial_method_wrapper(DexMethod* m, const ClassHierarchy& ch) {
  auto code = m->get_code();
  if (code == nullptr) return nullptr;
  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  auto end = ii.end();
  while (it != end && opcode::is_load_param(it->insn->opcode())) {
    ++it;
  }

  bool is_direct = it->insn->opcode() == OPCODE_INVOKE_DIRECT;
  bool is_static = it->insn->opcode() == OPCODE_INVOKE_STATIC;
  if (!is_direct && !is_static) return nullptr;

  auto invoke = it->insn;
  auto method = invoke->get_method();
  if (is_static) {
    method = resolve_static(type_class(method->get_class()),
        method->get_name(), method->get_proto());
  }
  if (!method) return nullptr;
  if (!method->is_concrete()) return nullptr;

  const auto method_def = static_cast<DexMethod*>(method);
  auto collision = find_collision_excepting(ch,
                                            method_def,
                                            method_def->get_name(),
                                            method_def->get_proto(),
                                            type_class(method_def->get_class()),
                                            true,
                                            true);
  if (collision) {
    TRACE(SYNT,
          5,
          "wrapper blocked:%s\nwrapped method:%s\nconflicts with:%s\n",
          SHOW(m),
          SHOW(method_def),
          SHOW(collision));
    return nullptr;
  }
  if (!passes_args_through(invoke, *code)) return nullptr;
  ++it;
  if (it == end) return nullptr;

  if (is_move_result(it->insn->opcode())) {
    ++it;
    if (it == end) return nullptr;
    if (!is_return_value(it->insn->opcode())) return nullptr;
    ++it;
    if (it != end) return nullptr; // exception handling code
    return method_def;
  }
  if (it->insn->opcode() == OPCODE_RETURN_VOID) {
    ++it;
    if (it != end) return nullptr; // exception handling code
    return method_def;
  }
  return nullptr;
}

/*
 * Matches the pattern:
 *   invoke-direct {v0...} Lclass;.<init>
 *   return-void
 */
DexMethod* trivial_ctor_wrapper(DexMethod* m) {
  auto code = m->get_code();
  if (code == nullptr) return nullptr;
  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  auto end = ii.end();
  while (it != end && opcode::is_load_param(it->insn->opcode())) {
    ++it;
  }

  if (it->insn->opcode() != OPCODE_INVOKE_DIRECT) {
    TRACE(SYNT, 5, "Rejecting, not direct: %s\n", SHOW(m));
    return nullptr;
  }
  auto invoke = it->insn;
  if (!passes_args_through(invoke, *code, 1)) {
    TRACE(SYNT, 5, "Rejecting, not passthrough: %s\n", SHOW(m));
    return nullptr;
  }
  ++it;
  if (it == end) return nullptr;
  if (it->insn->opcode() != OPCODE_RETURN_VOID) return nullptr;
  auto method = invoke->get_method();
  if (!method->is_concrete() ||
      !is_constructor(static_cast<DexMethod*>(method))) {
    return nullptr;
  }
  return static_cast<DexMethod*>(method);
}

struct WrapperMethods {
  std::unordered_map<DexMethod*, DexField*> getters;
  std::unordered_map<DexMethod*, DexMethod*> wrappers;
  std::unordered_map<DexMethod*, DexMethod*> ctors;
  std::unordered_map<DexMethod*, std::pair<DexMethod*, int>> wrapped;
  std::unordered_set<DexMethod*> keepers;
  std::unordered_set<DexMethod*> methods_to_update;
  std::unordered_set<DexMethod*> promoted_to_static;
  bool next_pass = false;
};

/**
 * Find and remove wrappers to wrappers. This removes loops and chain of
 * wrappers leaving only one level (and the first level) of wrappers
 */
void purge_wrapped_wrappers(WrapperMethods& ssms) {
  std::vector<DexMethod*> remove;
  for (auto& p : ssms.wrappers) {
    if (ssms.wrappers.count(p.second)) {
      remove.emplace_back(p.second);
    }
    if (ssms.getters.count(p.second)) {
      // a getter is a leaf so we remove it and we'll likely pick
      // it up next pass
      TRACE(SYNT, 5, "Removing wrapped getter: %s\n", SHOW(p.second));
      ssms.getters.erase(p.second);
      ssms.next_pass = true;
    }
  }
  for (auto meth : remove) {
    auto wrapped = ssms.wrapped.find(ssms.wrappers[meth]);
    if (wrapped != ssms.wrapped.end()) {
      if (--wrapped->second.second == 0) {
        TRACE(SYNT, 5, "Removing wrapped: %s\n", SHOW(ssms.wrappers[meth]));
        ssms.wrapped.erase(wrapped);
      }
    }
    TRACE(SYNT, 5, "Removing wrapper: %s\n", SHOW(meth));
    ssms.wrappers.erase(meth);
  }
  ssms.next_pass = ssms.next_pass || remove.size() > 0;
}

WrapperMethods analyze(const ClassHierarchy& ch,
                       const std::vector<DexClass*>& classes,
                       const SynthConfig& synthConfig) {
  WrapperMethods ssms;
  for (auto cls : classes) {
    for (auto dmethod : cls->get_dmethods()) {
      // constructors are special and all we can remove are synthetic ones
      if (synthConfig.remove_constructors && is_synthetic(dmethod) &&
          is_constructor(dmethod)) {
        auto ctor = trivial_ctor_wrapper(dmethod);
        if (ctor) {
          TRACE(SYNT, 2, "Trivial constructor wrapper: %s\n", SHOW(dmethod));
          TRACE(SYNT, 2, "  Calls constructor: %s\n", SHOW(ctor));
          ssms.ctors.emplace(dmethod, ctor);
        }
        continue;
      }
      if (is_constructor(dmethod)) continue;

      if (is_static_synthetic(dmethod)) {
        auto field = trivial_get_field_wrapper(dmethod);
        if (field) {
          TRACE(SYNT, 2, "Static trivial getter: %s\n", SHOW(dmethod));
          TRACE(SYNT, 2, "  Gets field: %s\n", SHOW(field));
          ssms.getters.emplace(dmethod, field);
          continue;
        }
        auto sfield = trivial_get_static_field_wrapper(dmethod);
        if (sfield) {
          TRACE(SYNT, 2, "Static trivial static field getter: %s\n",
          SHOW(dmethod));
          TRACE(SYNT, 2, "  Gets static field: %s\n", SHOW(sfield));
          ssms.getters.emplace(dmethod, sfield);
          continue;
        }
      }

      if (can_optimize(dmethod, synthConfig)) {
        auto method = trivial_method_wrapper(dmethod, ch);
        if (method) {
          // this is not strictly needed but to avoid changing visibility of
          // virtuals we are skipping a wrapper to a virtual.
          // Incidentally we have no single method falling in that bucket
          // at this time
          if (method->is_virtual()) continue;

          TRACE(SYNT, 2, "Static trivial method wrapper: %s\n", SHOW(dmethod));
          TRACE(SYNT, 2, "  Calls method: %s\n", SHOW(method));
          ssms.wrappers.emplace(dmethod, method);
          if (!is_static(method)) {
            auto wrapped = ssms.wrapped.find(method);
            if (wrapped == ssms.wrapped.end()) {
              ssms.wrapped.emplace(method, std::make_pair(dmethod, 1));
            } else {
              wrapped->second.second++;
            }
          }
        }
      }
    }
    if (debug) {
      // Static synthetics should never be virtual.
      for (auto vmethod : cls->get_vmethods()) {
        (void)vmethod;
        redex_assert(!is_static_synthetic(vmethod));
      }
    }
  }
  purge_wrapped_wrappers(ssms);
  return ssms;
}

IRInstruction* make_iget(DexField* field, uint16_t src) {
  auto const opcode = [&]() {
    switch (type_to_datatype(field->get_type())) {
    case DataType::Array:
    case DataType::Object:
      return OPCODE_IGET_OBJECT;
    case DataType::Boolean:
      return OPCODE_IGET_BOOLEAN;
    case DataType::Byte:
      return OPCODE_IGET_BYTE;
    case DataType::Char:
      return OPCODE_IGET_CHAR;
    case DataType::Short:
      return OPCODE_IGET_SHORT;
    case DataType::Int:
    case DataType::Float:
      return OPCODE_IGET;
    case DataType::Long:
    case DataType::Double:
      return OPCODE_IGET_WIDE;
    case DataType::Void:
      redex_assert(false);
    }
    not_reached();
  }();

  return (new IRInstruction(opcode))->set_field(field)->set_src(0, src);
}

IRInstruction* make_sget(DexField* field) {
  auto const opcode = opcode::sget_opcode_for_field(field);
  return (new IRInstruction(opcode))->set_field(field);
}

IROpcode move_result_to_pseudo(IROpcode op) {
  switch (op) {
  case OPCODE_MOVE_RESULT:
    return IOPCODE_MOVE_RESULT_PSEUDO;
  case OPCODE_MOVE_RESULT_OBJECT:
    return IOPCODE_MOVE_RESULT_PSEUDO_OBJECT;
  case OPCODE_MOVE_RESULT_WIDE:
    return IOPCODE_MOVE_RESULT_PSEUDO_WIDE;
  default:
    always_assert(false);
  }
}

void replace_getter_wrapper(IRCode* transform,
                            IRInstruction* insn,
                            IRInstruction* move_result,
                            DexField* field) {
  TRACE(SYNT, 2, "Optimizing getter wrapper call: %s\n", SHOW(insn));
  redex_assert(field->is_concrete());
  set_public(field);

  auto new_get = is_static(field)
                ? make_sget(field)
                : make_iget(field, insn->src(0));
  TRACE(SYNT, 2, "Created instruction: %s\n", SHOW(new_get));
  auto move_result_pseudo =
      (new IRInstruction(move_result_to_pseudo(move_result->opcode())))
          ->set_dest(move_result->dest());

  transform->replace_opcode(insn, {new_get, move_result_pseudo});
  transform->remove_opcode(move_result);
}

void update_invoke(IRCode* transform,
                   IRInstruction* insn,
                   DexMethod* method) {
  auto op = insn->opcode();
  auto new_invoke = [&] {
    redex_assert(op == OPCODE_INVOKE_STATIC || op == OPCODE_INVOKE_DIRECT);
    auto new_op = is_static(method) ? OPCODE_INVOKE_STATIC
                  : OPCODE_INVOKE_DIRECT;
    auto ret = new IRInstruction(new_op);
    ret->set_method(method)->set_arg_word_count(insn->arg_word_count());
    for (int i = 0; i < ret->arg_word_count(); i++) {
      ret->set_src(i, insn->src(i));
    }
    return ret;
  }();

  TRACE(SYNT, 2, "new instruction: %s\n", SHOW(new_invoke));
  transform->replace_opcode(insn, new_invoke);
}

bool can_update_wrappee(
    const ClassHierarchy& ch, DexMethod* wrappee, DexMethod* wrapper) {
  if (is_native(wrappee)) {
    // Can't change the signature of native methods.
    return false;
  }
  DexProto* old_proto = wrappee->get_proto();
  auto new_args = old_proto->get_args()->get_type_list();
  new_args.push_front(wrappee->get_class());
  DexProto* new_proto = DexProto::make_proto(
    old_proto->get_rtype(),
    DexTypeList::make_type_list(std::move(new_args)));
  auto new_name = wrappee->get_name();
  auto new_class = type_class(wrappee->get_class());
  if (find_collision(ch, new_name, new_proto, new_class, false)) {
    if (find_collision_excepting(
          ch,
          wrapper,
          new_name,
          new_proto,
          new_class,
          false /* is_virtual */,
          true /* check_direct */)) {
      return false;
    }
    return can_delete(wrapper);
  }
  return true;
}

bool replace_method_wrapper(const ClassHierarchy& ch,
                            IRCode* transform,
                            IRInstruction* insn,
                            DexMethod* wrapper,
                            DexMethod* wrappee,
                            WrapperMethods& ssms) {
  TRACE(SYNT, 2, "Optimizing method wrapper: %s\n", SHOW(insn));
  TRACE(SYNT, 3, "  wrapper:%p wrappee:%p\n", wrapper, wrappee);
  TRACE(SYNT, 3, "  wrapper: %s\n", SHOW(wrapper));
  TRACE(SYNT, 3, "  wrappee: %s\n", SHOW(wrappee));
  redex_assert(wrappee->is_concrete() && wrapper->is_concrete());

  if (is_static(wrapper) && !is_static(wrappee)) {
    assert(can_update_wrappee(ch, wrappee, wrapper));
    mutators::make_static(wrappee);
    ssms.promoted_to_static.insert(wrappee);
  }
  if (!is_private(wrapper)) {
    set_public(wrappee);
    if (wrapper->get_class() != wrappee->get_class()) {
      set_public(type_class(wrappee->get_class()));
    }
  }

  update_invoke(transform, insn, wrappee);
  return true;
}

void replace_ctor_wrapper(IRCode* transform,
                          IRInstruction* ctor_insn,
                          DexMethod* ctor) {
  TRACE(SYNT, 2, "Optimizing static ctor: %s\n", SHOW(ctor_insn));
  redex_assert(ctor->is_concrete());
  set_public(ctor);

  auto op = ctor_insn->opcode();
  auto new_ctor_call = [&] {
    redex_assert(op == OPCODE_INVOKE_DIRECT);
    auto ret = new IRInstruction(OPCODE_INVOKE_DIRECT);
    ret->set_method(ctor)->set_arg_word_count(
        ctor_insn->arg_word_count() - 1);
    for (int i = 0; i < ret->arg_word_count(); i++) {
      ret->set_src(i, ctor_insn->src(i));
    }
    return ret;
  }();

  TRACE(SYNT, 2, "new instruction: %s\n", SHOW(new_ctor_call));
  transform->replace_opcode(ctor_insn, new_ctor_call);
}

void replace_wrappers(const ClassHierarchy& ch,
                      DexMethod* caller_method,
                      WrapperMethods& ssms) {
  std::vector<std::tuple<IRInstruction*, IRInstruction*, DexField*>> getter_calls;
  std::vector<std::pair<IRInstruction*, DexMethod*>> wrapper_calls;
  std::vector<std::pair<IRInstruction*, DexMethod*>> wrapped_calls;
  std::vector<std::pair<IRInstruction*, DexMethod*>> ctor_calls;

  TRACE(SYNT, 4, "Replacing wrappers in %s\n", SHOW(caller_method));
  auto ii = InstructionIterable(caller_method->get_code());
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      // Replace calls to static getters and wrappers
      auto const callee =
          resolve_method(insn->get_method(), MethodSearch::Static);
      if (callee == nullptr) continue;

      auto const found_get = ssms.getters.find(callee);
      if (found_get != ssms.getters.end()) {
        auto next_it = std::next(it);
        auto const move_result = next_it->insn;
        if (!is_move_result(move_result->opcode())) {
          ssms.keepers.emplace(callee);
          continue;
        }
        auto field = found_get->second;
        getter_calls.emplace_back(insn, move_result, field);
        continue;
      }

      auto const found_wrap = ssms.wrappers.find(callee);
      if (found_wrap != ssms.wrappers.end()) {
        auto method = found_wrap->second;
        wrapper_calls.emplace_back(insn, method);
        continue;
      }
      always_assert_log(
        ssms.wrapped.find(callee) == ssms.wrapped.end(),
        "caller: %s\ncallee: %s\ninsn: %s\n",
        SHOW(caller_method), SHOW(callee), SHOW(insn));

      ssms.keepers.emplace(callee);
    } else if (insn->opcode() == OPCODE_INVOKE_DIRECT) {
      auto const callee =
          resolve_method(insn->get_method(), MethodSearch::Direct);
      if (callee == nullptr) continue;

      auto const found_get = ssms.getters.find(callee);
      if (found_get != ssms.getters.end()) {
        auto next_it = std::next(it);
        auto const move_result = next_it->insn;
        if (!is_move_result(move_result->opcode())) {
          ssms.keepers.emplace(callee);
          continue;
        }
        auto field = found_get->second;
        getter_calls.emplace_back(insn, move_result, field);
        continue;
      }

      auto const found_wrap = ssms.wrappers.find(callee);
      if (found_wrap != ssms.wrappers.end()) {
        auto method = found_wrap->second;
        wrapper_calls.emplace_back(insn, method);
        continue;
      }

      auto const found_wrappee = ssms.wrapped.find(callee);
      if (found_wrappee != ssms.wrapped.end()) {
        auto wrapper = found_wrappee->second.first;
        wrapped_calls.emplace_back(insn, wrapper);
        continue;
      }

      auto const found_ctor = ssms.ctors.find(callee);
      if (found_ctor != ssms.ctors.end()) {
        auto ctor = found_ctor->second;
        ctor_calls.emplace_back(insn, ctor);
        continue;
      }
    }
  }
  // Prune out wrappers that are invalid due to naming conflicts.
  std::unordered_set<DexMethod*> bad_wrappees;
  std::unordered_multimap<DexMethod*, DexMethod*> wrappees_to_wrappers;
  for (auto wpair : wrapper_calls) {
    auto call_inst = wpair.first;
    auto wrapper = static_cast<DexMethod*>(call_inst->get_method());
    auto wrappee = wpair.second;
    wrappees_to_wrappers.emplace(wrappee, wrapper);
    if (!can_update_wrappee(ch, wrappee, wrapper)) {
      bad_wrappees.emplace(wrappee);
    }
  }
  for (auto wpair : wrapped_calls) {
    auto call_inst = wpair.first;
    auto wrapper = wpair.second;
    auto wrappee = static_cast<DexMethod*>(call_inst->get_method());
    wrappees_to_wrappers.emplace(wrappee, wrapper);
    if (!can_update_wrappee(ch, wrappee, wrapper)) {
      bad_wrappees.emplace(wrappee);
    }
  }
  for (auto bw : bad_wrappees) {
    auto range = wrappees_to_wrappers.equal_range(bw);
    for (auto it = range.first; it != range.second; ++it) {
      ssms.keepers.emplace(it->second);
    }
  }
  wrapper_calls.erase(
    std::remove_if(
      wrapper_calls.begin(), wrapper_calls.end(),
      [&](const std::pair<IRInstruction*, DexMethod*>& p) {
        return bad_wrappees.count(p.second);
      }),
    wrapper_calls.end()
  );
  wrapped_calls.erase(
    std::remove_if(
      wrapped_calls.begin(), wrapped_calls.end(),
      [&](const std::pair<IRInstruction*, DexMethod*>& p) {
        return bad_wrappees.count(
            static_cast<DexMethod*>(p.first->get_method()));
      }),
    wrapped_calls.end()
  );
  // Fix up everything left.
  if (getter_calls.empty() && wrapper_calls.empty() && ctor_calls.empty() &&
      wrapped_calls.empty()) {
    return;
  }
  auto code = caller_method->get_code();
  for (auto g : getter_calls) {
    using std::get;
    replace_getter_wrapper(code, get<0>(g), get<1>(g), get<2>(g));
  }
  for (auto wpair : wrapper_calls) {
    auto call_inst = wpair.first;
    auto wrapper = static_cast<DexMethod*>(call_inst->get_method());
    auto wrappee = wpair.second;
    auto success =
      replace_method_wrapper(
        ch,
        &*code,
        call_inst,
        wrapper,
        wrappee,
        ssms);
    if (!success) {
      ssms.keepers.emplace(static_cast<DexMethod*>(wpair.first->get_method()));
    }
  }
  for (auto wpair : wrapped_calls) {
    auto call_inst = wpair.first;
    auto wrapper = wpair.second;
    auto wrappee = static_cast<DexMethod*>(call_inst->get_method());
    auto success =
      replace_method_wrapper(
        ch,
        &*code,
        call_inst,
        wrapper,
        wrappee,
        ssms);
    if (!success) {
      ssms.keepers.emplace(static_cast<DexMethod*>(wpair.first->get_method()));
    }
  }
  for (auto cpair : ctor_calls) {
    replace_ctor_wrapper(&*code, cpair.first, cpair.second);
  }
}

void remove_dead_methods(
  WrapperMethods& ssms, const SynthConfig& synthConfig, SynthMetrics& metrics) {
  bool any_remove = false;
  size_t synth_removed = 0;
  size_t other_removed = 0;
  size_t pub_meth = 0;
  auto remove_meth = [&](DexMethod* meth) {
    redex_assert(meth->is_concrete());
    if (!can_remove(meth, synthConfig)) {
      return;
    }
    if (ssms.keepers.count(meth)) {
      TRACE(SYNT, 2, "Retaining method: %s\n", SHOW(meth));
      return;
    }
    if (!can_delete(meth)) {
      TRACE(SYNT, 2, "Do not strip: %s\n", SHOW(meth));
      return;
    }

    TRACE(SYNT, 2, "Removing method: %s\n", SHOW(meth));
    if (is_public(meth)) pub_meth++;
    auto cls = type_class(meth->get_class());
    cls->remove_method(meth);
    is_synthetic(meth) ? synth_removed++ : other_removed++;
  };

  for (auto const gp : ssms.getters) {
    remove_meth(gp.first);
  }
  any_remove = any_remove || (synth_removed && other_removed);
  TRACE(SYNT, 3, "any_remove = %d\n", any_remove);
  TRACE(SYNT, 3, "synth_removed = %d\n", synth_removed);
  TRACE(SYNT, 3, "other_removed = %d\n", other_removed);
  if (synth_removed) {
    TRACE(SYNT, 1, "Synthetic getters removed %ld\n", synth_removed);
  }
  if (other_removed) {
    TRACE(SYNT, 1, "Other getters removed %ld\n", other_removed);
  }
  if (pub_meth) {
    TRACE(SYNT, 1, "Public getters removed %ld\n", pub_meth);
  }

  metrics.getters_removed_count += (synth_removed + other_removed + pub_meth);

  synth_removed = 0;
  other_removed = 0;
  pub_meth = 0;
  for (auto const wp : ssms.wrappers) {
    remove_meth(wp.first);
  }
  any_remove = any_remove || (synth_removed && other_removed);
  if (synth_removed) {
    TRACE(SYNT, 1, "Synthetic wrappers removed %ld\n", synth_removed);
  }
  if (other_removed) {
    TRACE(SYNT, 1, "Other wrappers removed %ld\n", other_removed);
  }
  if (pub_meth) {
    TRACE(SYNT, 1, "Public wrappers removed %ld\n", pub_meth);
  }

  metrics.wrappers_removed_count += (synth_removed + other_removed + pub_meth);

  synth_removed = 0;
  other_removed = 0;
  pub_meth = 0;
  for (auto const ct : ssms.ctors) {
    remove_meth(ct.first);
  }
  any_remove = any_remove || (synth_removed && other_removed);
  if (synth_removed) {
    TRACE(SYNT, 1, "Synthetic constructor removed %ld\n", synth_removed);
  }
  if (pub_meth) {
    TRACE(SYNT, 1, "Public constructor removed %ld\n", pub_meth);
  }

  metrics.ctors_removed_count += (synth_removed + pub_meth);

  redex_assert(other_removed == 0);
  ssms.next_pass = ssms.next_pass && any_remove;
}

void do_transform(const ClassHierarchy& ch,
                  const std::vector<DexClass*>& classes,
                  WrapperMethods& ssms,
                  const SynthConfig& synthConfig,
                  SynthMetrics& metrics) {
  // remove wrappers.  build a vector ahead of time to ensure we only visit each
  // method once, even if we mutate the class method lists such that we'd hit
  // something a second time.
  std::vector<DexMethod*> methods;
  for (auto const& cls : classes) {
    for (auto const& dm : cls->get_dmethods()) {
      methods.emplace_back(dm);
    }
    for (auto const& vm : cls->get_vmethods()) {
      methods.emplace_back(vm);
    }
  }
  for (auto const& m : methods) {
    if (m->get_code()) {
      replace_wrappers(ch, m, ssms);
    }
  }
  // check that invokes to promoted static method is correct
  walk::parallel::code(classes, [&](DexMethod* meth, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto* insn = mie.insn;
      auto opcode = insn->opcode();
      if (opcode != OPCODE_INVOKE_DIRECT) {
        continue;
      }
      auto wrappee =
            resolve_method(insn->get_method(), MethodSearch::Direct);
      if (wrappee == nullptr || ssms.promoted_to_static.count(wrappee) == 0) {
        continue;
      }
      // change the opcode to invoke-static
      insn->set_opcode(OPCODE_INVOKE_STATIC);
      TRACE(SYNT, 3,
            "Updated invoke on promoted to static %s\n in method %s",
            SHOW(wrappee),
            SHOW(meth));
    }
  });
  remove_dead_methods(ssms, synthConfig, metrics);
}

bool trace_analysis(WrapperMethods& ssms) {
  DEBUG_ONLY size_t synth = 0;
  DEBUG_ONLY size_t others = 0;
  for (auto it : ssms.getters) {
    auto meth = it.first;
    is_synthetic(meth) ? synth++ : others++;
  }
  TRACE(SYNT, 3, "synth getters %ld\n", synth);
  TRACE(SYNT, 3, "other getters %ld\n", others);

  synth = 0;
  others = 0;
  for (auto it : ssms.ctors) {
    auto meth = it.first;
    if (is_synthetic(meth)) {
      synth++;
    } else {
      others++;
    }
  }
  TRACE(SYNT, 3, "synth ctors %ld\n", synth);
  TRACE(SYNT, 3, "other ctors %ld\n", others);

  synth = 0;
  others = 0;
  for (auto it : ssms.wrappers) {
    auto meth = it.first;
    is_synthetic(meth) ? synth++ : others++;
  }
  TRACE(SYNT, 3, "synth methods %ld\n", synth);
  TRACE(SYNT, 3, "other methods %ld\n", others);
  return true;
}

bool optimize(const ClassHierarchy& ch,
              const std::vector<DexClass*>& classes,
              const SynthConfig& synthConfig,
              SynthMetrics& metrics) {
  auto ssms = analyze(ch, classes, synthConfig);
  redex_assert(trace_analysis(ssms));
  do_transform(ch, classes, ssms, synthConfig, metrics);
  return ssms.next_pass;
}

void SynthPass::run_pass(DexStoresVector& stores,
                         ConfigFiles& /* conf */,
                         PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(SYNT, 1, "SynthPass not run because no ProGuard configuration was provided.");
    return;
  }
  Scope scope = build_class_scope(stores);
  ClassHierarchy ch = build_type_hierarchy(scope);
  SynthMetrics metrics;
  int passes = 0;
  do {
    TRACE(SYNT, 1, "Synth removal, pass %d\n", passes);
    bool more_opt_needed = optimize(ch, scope, m_pass_config, metrics);
    if (!more_opt_needed) break;
  } while (++passes < m_pass_config.max_passes);

  mgr.incr_metric(
    METRIC_GETTERS_REMOVED, metrics.getters_removed_count);
  mgr.incr_metric(
    METRIC_WRAPPERS_REMOVED, metrics.wrappers_removed_count);
  mgr.incr_metric(
    METRIC_CTORS_REMOVED, metrics.ctors_removed_count);
}

static SynthPass s_pass;
