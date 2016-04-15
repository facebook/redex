/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOpcode.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "PassManager.h"
#include "Transform.h"
#include "ReachableClasses.h"
#include "walkers.h"

#include "SynthConfig.h"

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
 *   iget-TYPE vA, vB, FIELD
 *   return-TYPE vA
 */
DexField* trivial_get_field_wrapper(DexMethod* m) {
  DexCode* code = m->get_code();
  if (code == nullptr) return nullptr;

  auto& insns = code->get_instructions();
  auto it = insns.begin();
  if (!is_iget((*it)->opcode())) return nullptr;

  auto iget = static_cast<DexOpcodeField*>(*it);
  uint16_t iget_dest = iget->dest();
  ++it;

  if (!is_return_value((*it)->opcode())) return nullptr;

  uint16_t ret_reg = (*it)->src(0);
  if (ret_reg != iget_dest) return nullptr;
  ++it;

  if (it != insns.end()) return nullptr;

  // Check to make sure we have a concrete field reference.
  auto def = resolve_field(iget->field(), FieldSearch::Instance);
  if (def == nullptr) return nullptr;
  if (!def->is_concrete()) {
    return nullptr;
  }

  return def;
}

/*
 * Matches the pattern:
 *   sget-TYPE vA, FIELD
 *   return-TYPE vA
 */
DexField* trivial_get_static_field_wrapper(DexMethod* m) {
  DexCode* code = m->get_code();
  if (code == nullptr) return nullptr;

  auto& insns = code->get_instructions();
  auto it = insns.begin();
  if (!is_sget((*it)->opcode())) return nullptr;

  auto sget = static_cast<DexOpcodeField*>(*it);
  uint16_t sget_dest = sget->dest();
  ++it;

  if (!is_return_value((*it)->opcode())) return nullptr;

  uint16_t ret_reg = (*it)->src(0);
  if (ret_reg != sget_dest) return nullptr;
  ++it;

  if (it != insns.end()) return nullptr;

  // Check to make sure we have a concrete field reference.
  auto def = resolve_field(sget->field(), FieldSearch::Static);
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
DexMethod* trivial_method_wrapper(DexMethod* m) {
  DexCode* code = m->get_code();
  if (code == nullptr) return nullptr;
  auto& insns = code->get_instructions();
  auto it = insns.begin();

  bool is_direct = (*it)->opcode() == OPCODE_INVOKE_DIRECT;
  bool is_static = (*it)->opcode() == OPCODE_INVOKE_STATIC;
  if (!is_direct && !is_static) return nullptr;

  auto invoke = static_cast<DexOpcodeMethod*>(*it);
  auto method = invoke->get_method();
  if (is_static) {
    method = resolve_static(type_class(method->get_class()),
        method->get_name(), method->get_proto());
  }
  if (!method) return nullptr;
  if (!method->is_concrete()) return nullptr;

  auto collision = find_collision_excepting(method,
                                            method->get_name(),
                                            method->get_proto(),
                                            type_class(method->get_class()),
                                            true,
                                            true);
  if (collision) {
    TRACE(SYNT,
          5,
          "wrapper blocked:%s\nwrapped method:%s\nconflicts with:%s\n",
          SHOW(m),
          SHOW(method),
          SHOW(collision));
    return nullptr;
  }
  if (!passes_args_through(invoke, code)) return nullptr;
  if (++it == insns.end()) return nullptr;

  if (is_move_result((*it)->opcode())) {
    if (++it == insns.end()) return nullptr;
    if (!is_return_value((*it)->opcode())) return nullptr;
    if (++it != insns.end()) return nullptr; // exception handling code
    return method;
  }
  if ((*it)->opcode() == OPCODE_RETURN_VOID) {
    if (++it != insns.end()) return nullptr; // exception handling code
    return method;
  }
  return nullptr;
}

/*
 * Matches the pattern:
 *   invoke-direct {v0...} Lclass;.<init>
 *   return-void
 */
DexMethod* trivial_ctor_wrapper(DexMethod* m) {
  DexCode* code = m->get_code();
  if (code == nullptr) return nullptr;
  auto& insns = code->get_instructions();
  auto it = insns.begin();
  auto invoke = static_cast<DexOpcodeMethod*>(*it);
  if (invoke->opcode() != OPCODE_INVOKE_DIRECT) {
    TRACE(SYNT, 5, "Rejecting, not direct: %s\n", SHOW(m));
    return nullptr;
  }
  if (!passes_args_through(invoke, code, 1)) {
    TRACE(SYNT, 5, "Rejecting, not passthrough: %s\n", SHOW(m));
    return nullptr;
  }
  if (++it == insns.end()) return nullptr;
  if ((*it)->opcode() != OPCODE_RETURN_VOID) return nullptr;
  auto method = invoke->get_method();
  if (!method->is_concrete() || !is_constructor(method)) return nullptr;
  return method;
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

WrapperMethods analyze(const std::vector<DexClass*>& classes,
                       const SynthConfig& synthConfig) {
  WrapperMethods ssms;
  for (auto cls : classes) {
    for (auto dmethod : cls->get_dmethods()) {
      // constructors are special and all we can remove are synthetic ones
      if (is_synthetic(dmethod) && is_constructor(dmethod)) {
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
        auto method = trivial_method_wrapper(dmethod);
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
        assert(!is_static_synthetic(vmethod));
      }
    }
  }
  purge_wrapped_wrappers(ssms);
  return ssms;
}

DexOpcode* make_iget(DexField* field, uint8_t dest, uint8_t src) {
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
      assert(false);
    }
    not_reached();
  }();

  if (dest > 15 || src > 15) return nullptr;
  return (new DexOpcodeField(opcode, field))->set_dest(dest)->set_src(0, src);
}

DexOpcode* make_sget(DexField* field, uint8_t dest) {
  auto const opcode = [&]() {
    switch (type_to_datatype(field->get_type())) {
    case DataType::Array:
    case DataType::Object:
      return OPCODE_SGET_OBJECT;
    case DataType::Boolean:
      return OPCODE_SGET_BOOLEAN;
    case DataType::Byte:
      return OPCODE_SGET_BYTE;
    case DataType::Char:
      return OPCODE_SGET_CHAR;
    case DataType::Short:
      return OPCODE_SGET_SHORT;
    case DataType::Int:
    case DataType::Float:
      return OPCODE_SGET;
    case DataType::Long:
    case DataType::Double:
      return OPCODE_SGET_WIDE;
    case DataType::Void:
      assert(false);
    }
    not_reached();
  }();

  return (new DexOpcodeField(opcode, field))->set_dest(dest);
}

bool replace_getter_wrapper(MethodTransformer& transform,
                            DexOpcodeMethod* meth_insn,
                            DexOpcode* move_result,
                            DexField* field) {
  TRACE(SYNT, 2, "Optimizing getter wrapper call: %s\n", SHOW(meth_insn));
  assert(field->is_concrete());
  set_public(field);

  auto invoke_src = meth_insn->src(0);
  auto move_result_dest = move_result->dest();

  TRACE(SYNT,
        2,
        "Creating instruction %s %d %d\n",
        SHOW(field),
        move_result_dest,
        invoke_src);
  auto new_get = (field->get_access() & ACC_STATIC)
                ? make_sget(field, move_result_dest)
                : make_iget(field, move_result_dest, invoke_src);
  if (!new_get) return false;
  TRACE(SYNT, 2, "Created instruction: %s\n", SHOW(new_get));

  transform->replace_opcode(meth_insn, new_get);
  transform->remove_opcode(move_result);
  return true;
}

void update_invoke(MethodTransformer& transform,
                   DexOpcodeMethod* meth_insn,
                   DexMethod* method) {
  auto op = meth_insn->opcode();
  auto new_invoke = [&] {
    if (op == OPCODE_INVOKE_DIRECT_RANGE) {
      auto new_op = is_static(method) ? OPCODE_INVOKE_STATIC_RANGE
                    : OPCODE_INVOKE_DIRECT_RANGE;
      auto ret = new DexOpcodeMethod(new_op, method, 0);
      ret->set_range_base(meth_insn->range_base())
          ->set_range_size(meth_insn->range_size());
      return ret;
    } else {
      assert(op == OPCODE_INVOKE_STATIC || op == OPCODE_INVOKE_DIRECT);
      auto new_op = is_static(method) ? OPCODE_INVOKE_STATIC
                    : OPCODE_INVOKE_DIRECT;
      auto ret = new DexOpcodeMethod(new_op, method, 0);
      ret->set_arg_word_count(meth_insn->arg_word_count());
      for (int i = 0; i < 5; i++) {
        ret->set_src(i, meth_insn->src(i));
      }
      return ret;
    }
  }();

  TRACE(SYNT, 2, "new instruction: %s\n", SHOW(new_invoke));
  transform->replace_opcode(meth_insn, new_invoke);
}

bool can_update_wrappee(DexMethod* wrappee, DexMethod* wrapper) {
  if (is_native(wrappee)) {
    // Can't change the signature of native methods.
    return false;
  }
  DexProto* old_proto = wrappee->get_proto();
  std::list<DexType*> new_args = old_proto->get_args()->get_type_list();
  new_args.push_front(wrappee->get_class());
  DexProto* new_proto = DexProto::make_proto(
    old_proto->get_rtype(),
    DexTypeList::make_type_list(std::move(new_args)));
  if (find_collision_excepting(wrapper,
                               wrappee->get_name(),
                               new_proto,
                               type_class(wrappee->get_class()),
                               false /* is_virtual */,
                               true /* check_direct */)) {
    return false;
  }
  return true;
}

/**
 * If the wrappee wasn't initially static we need to make `this` an explicit
 * parameter.
 */
void make_static_and_update_args(DexMethod* wrappee, DexMethod* wrapper) {
  assert(can_update_wrappee(wrappee, wrapper));
  DexProto* old_proto = wrappee->get_proto();
  std::list<DexType*> new_args = old_proto->get_args()->get_type_list();
  new_args.push_front(wrappee->get_class());
  DexProto* new_proto = DexProto::make_proto(
    old_proto->get_rtype(),
    DexTypeList::make_type_list(std::move(new_args)));
  wrappee->change_proto(new_proto);
  auto& dmethods = type_class(wrappee->get_class())->get_dmethods();
  dmethods.sort(compare_dexmethods);
  wrappee->set_access(wrappee->get_access() | ACC_STATIC);
}

bool replace_method_wrapper(MethodTransformer& transform,
                            DexOpcodeMethod* meth_insn,
                            DexMethod* wrapper,
                            DexMethod* wrappee,
                            WrapperMethods& ssms) {
  TRACE(SYNT, 2, "Optimizing method wrapper: %s\n", SHOW(meth_insn));
  TRACE(SYNT, 3, "  wrapper:%p wrappee:%p\n", wrapper, wrappee);
  TRACE(SYNT, 3, "  wrapper: %s\n", SHOW(wrapper));
  TRACE(SYNT, 3, "  wrappee: %s\n", SHOW(wrappee));
  assert(wrappee->is_concrete() && wrapper->is_concrete());

  if (is_static(wrapper) && !is_static(wrappee)) {
    make_static_and_update_args(wrappee, wrapper);
    ssms.promoted_to_static.insert(wrappee);
  }
  if (!is_private(wrapper)) {
    set_public(wrappee);
    if (wrapper->get_class() != wrappee->get_class()) {
      set_public(type_class(wrappee->get_class()));
    }
  }

  update_invoke(transform, meth_insn, wrappee);
  return true;
}

void replace_ctor_wrapper(MethodTransformer& transform,
                          DexOpcodeMethod* ctor_insn,
                          DexMethod* ctor) {
  TRACE(SYNT, 2, "Optimizing static ctor: %s\n", SHOW(ctor_insn));
  assert(ctor->is_concrete());
  set_public(ctor);

  auto op = ctor_insn->opcode();
  auto new_ctor_call = [&] {
    if (op == OPCODE_INVOKE_DIRECT_RANGE) {
      auto ret = new DexOpcodeMethod(OPCODE_INVOKE_DIRECT_RANGE, ctor, 0);
      ret->set_range_base(ctor_insn->range_base())
          ->set_range_size(ctor_insn->range_size() - 1);
      return ret;
    } else {
      assert(op == OPCODE_INVOKE_DIRECT);
      auto ret = new DexOpcodeMethod(OPCODE_INVOKE_DIRECT, ctor, 0);
      ret->set_arg_word_count(ctor_insn->arg_word_count() - 1);
      for (int i = 0; i < 5; i++) {
        ret->set_src(i, ctor_insn->src(i));
      }
      return ret;
    }
  }();

  TRACE(SYNT, 2, "new instruction: %s\n", SHOW(new_ctor_call));
  transform->replace_opcode(ctor_insn, new_ctor_call);
}

void replace_wrappers(DexMethod* caller_method,
                      DexCode* code,
                      WrapperMethods& ssms) {
  std::vector<std::tuple<DexOpcodeMethod*, DexOpcode*, DexField*>> getter_calls;
  std::vector<std::pair<DexOpcodeMethod*, DexMethod*>> wrapper_calls;
  std::vector<std::pair<DexOpcodeMethod*, DexMethod*>> wrapped_calls;
  std::vector<std::pair<DexOpcodeMethod*, DexMethod*>> ctor_calls;

  auto& insns = code->get_instructions();
  for (auto it = insns.begin(); it != insns.end(); ++it) {
    auto insn = *it;
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      // Replace calls to static getters and wrappers
      auto const meth_insn = static_cast<DexOpcodeMethod*>(insn);
      auto const callee = resolve_method(
          meth_insn->get_method(), MethodSearch::Static);
      if (callee == nullptr) continue;

      auto const found_get = ssms.getters.find(callee);
      if (found_get != ssms.getters.end()) {
        auto const move_result = *(std::next(it));
        if (!is_move_result(move_result->opcode())) {
          ssms.keepers.emplace(callee);
          continue;
        }
        auto field = found_get->second;
        getter_calls.emplace_back(meth_insn, move_result, field);
        continue;
      }

      auto const found_wrap = ssms.wrappers.find(callee);
      if (found_wrap != ssms.wrappers.end()) {
        auto method = found_wrap->second;
        wrapper_calls.emplace_back(meth_insn, method);
        continue;
      }
      assert(ssms.wrapped.find(callee) == ssms.wrapped.end());

      ssms.keepers.emplace(callee);
    } else if (insn->opcode() == OPCODE_INVOKE_DIRECT ||
               insn->opcode() == OPCODE_INVOKE_DIRECT_RANGE) {
      auto const meth_insn = static_cast<DexOpcodeMethod*>(insn);
      auto const callee =
          resolve_method(meth_insn->get_method(), MethodSearch::Direct);
      if (callee == nullptr) continue;

      auto const found_get = ssms.getters.find(callee);
      if (found_get != ssms.getters.end()) {
        auto const move_result = *(std::next(it));
        if (!is_move_result(move_result->opcode())) {
          ssms.keepers.emplace(callee);
          continue;
        }
        auto field = found_get->second;
        getter_calls.emplace_back(meth_insn, move_result, field);
        continue;
      }

      auto const found_wrap = ssms.wrappers.find(callee);
      if (found_wrap != ssms.wrappers.end()) {
        auto method = found_wrap->second;
        wrapper_calls.emplace_back(meth_insn, method);
        continue;
      }

      auto const found_wrappee = ssms.wrapped.find(callee);
      if (found_wrappee != ssms.wrapped.end()) {
        auto wrapper = found_wrappee->second.first;
        wrapped_calls.emplace_back(meth_insn, wrapper);
        continue;
      }

      auto const found_ctor = ssms.ctors.find(callee);
      if (found_ctor != ssms.ctors.end()) {
        auto ctor = found_ctor->second;
        ctor_calls.emplace_back(meth_insn, ctor);
        continue;
      }
    } else if (insn->opcode() == OPCODE_INVOKE_STATIC_RANGE) {
      // We don't handle this yet, but it's not hard.
      auto const meth_insn = static_cast<DexOpcodeMethod*>(insn);
      auto const callee = resolve_method(
          meth_insn->get_method(), MethodSearch::Static);
      if (callee == nullptr) continue;
      ssms.keepers.emplace(callee);
    }
  }
  // Prune out wrappers that are invalid due to naming conflicts.
  std::unordered_set<DexMethod*> bad_wrappees;
  std::unordered_multimap<DexMethod*, DexMethod*> wrappees_to_wrappers;
  for (auto wpair : wrapper_calls) {
    auto call_inst = wpair.first;
    auto wrapper = call_inst->get_method();
    auto wrappee = wpair.second;
    wrappees_to_wrappers.emplace(wrappee, wrapper);
    if (!can_update_wrappee(wrappee, wrapper)) {
      bad_wrappees.emplace(wrappee);
    }
  }
  for (auto wpair : wrapped_calls) {
    auto call_inst = wpair.first;
    auto wrapper = wpair.second;
    auto wrappee = call_inst->get_method();
    wrappees_to_wrappers.emplace(wrappee, wrapper);
    if (!can_update_wrappee(wrappee, wrapper)) {
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
      [&](const std::pair<DexOpcodeMethod*, DexMethod*>& p) {
        return bad_wrappees.count(p.second);
      }),
    wrapper_calls.end()
  );
  wrapped_calls.erase(
    std::remove_if(
      wrapped_calls.begin(), wrapped_calls.end(),
      [&](const std::pair<DexOpcodeMethod*, DexMethod*>& p) {
        return bad_wrappees.count(p.first->get_method());
      }),
    wrapped_calls.end()
  );
  // Fix up everything left.
  if (getter_calls.empty() && wrapper_calls.empty() && ctor_calls.empty() &&
      wrapped_calls.empty()) {
    return;
  }
  MethodTransformer transform(caller_method);
  for (auto g : getter_calls) {
    using std::get;
    if (!replace_getter_wrapper(transform, get<0>(g), get<1>(g), get<2>(g))) {
      ssms.keepers.emplace(get<0>(g)->get_method());
    }
  }
  for (auto wpair : wrapper_calls) {
    auto call_inst = wpair.first;
    auto wrapper = call_inst->get_method();
    auto wrappee = wpair.second;
    auto success =
      replace_method_wrapper(
        transform,
        call_inst,
        wrapper,
        wrappee,
        ssms);
    if (!success) {
      ssms.keepers.emplace(wpair.first->get_method());
    }
  }
  for (auto wpair : wrapped_calls) {
    auto call_inst = wpair.first;
    auto wrapper = wpair.second;
    auto wrappee = call_inst->get_method();
    auto success =
      replace_method_wrapper(
        transform,
        call_inst,
        wrapper,
        wrappee,
        ssms);
    if (!success) {
      ssms.keepers.emplace(wpair.first->get_method());
    }
  }
  for (auto cpair : ctor_calls) {
    replace_ctor_wrapper(transform, cpair.first, cpair.second);
  }
}

void remove_dead_methods(WrapperMethods& ssms, const SynthConfig& synthConfig) {
  bool any_remove = false;
  size_t synth_removed = 0;
  size_t other_removed = 0;
  size_t pub_meth = 0;
  auto remove_meth = [&](DexMethod* meth) {
    assert(meth->is_concrete());
    if (!can_remove(meth, synthConfig)) {
      return;
    }
    if (ssms.keepers.count(meth)) {
      TRACE(SYNT, 2, "Retaining method: %s\n", SHOW(meth));
      return;
    }
    if (do_not_strip(meth)) {
      TRACE(SYNT, 2, "Do not strip: %s\n", SHOW(meth));
      return;
    }

    TRACE(SYNT, 2, "Removing method: %s\n", SHOW(meth));
    if (is_public(meth)) pub_meth++;
    auto cls = type_class(meth->get_class());
    cls->get_dmethods().remove(meth);
    meth->get_access() & ACC_SYNTHETIC ? synth_removed++ : other_removed++;
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
  assert(other_removed == 0);
  ssms.next_pass = ssms.next_pass && any_remove;
}

void transform(const std::vector<DexClass*>& classes,
               WrapperMethods& ssms, const SynthConfig& synthConfig) {
  // remove wrappers
  walk_code(
      classes,
      [](DexMethod*) { return true; },
      [&](DexMethod* m, DexCode* code) { replace_wrappers(m, code, ssms); });
  // check that invokes to promoted static method is correct
  walk_opcodes(
      classes,
      [](DexMethod*) { return true; },
      [&](DexMethod* meth, DexOpcode* insn) {
        auto opcode = insn->opcode();
        if (opcode != OPCODE_INVOKE_DIRECT &&
            opcode != OPCODE_INVOKE_DIRECT_RANGE) {
          return;
        }
        auto wrappee = static_cast<DexOpcodeMethod*>(insn)->get_method();
        if (ssms.promoted_to_static.count(wrappee) == 0) {
          return;
        }
        // change the opcode to invoke-static
        insn->set_opcode(opcode == OPCODE_INVOKE_DIRECT ?
            OPCODE_INVOKE_STATIC : OPCODE_INVOKE_STATIC_RANGE);
        TRACE(SYNT, 3,
            "Updated invoke on promoted to static %s\n in method %s",
            SHOW(wrappee), SHOW(meth));
      });
  remove_dead_methods(ssms, synthConfig);
}

bool trace_analysis(WrapperMethods& ssms) {
  DEBUG_ONLY size_t synth = 0;
  DEBUG_ONLY size_t others = 0;
  for (auto it : ssms.getters) {
    auto meth = it.first;
    meth->get_access() & ACC_SYNTHETIC ? synth++ : others++;
  }
  TRACE(SYNT, 3, "synth getters %ld\n", synth);
  TRACE(SYNT, 3, "other getters %ld\n", others);

  synth = 0;
  others = 0;
  for (auto it : ssms.ctors) {
    auto meth = it.first;
    if (meth->get_access() & ACC_SYNTHETIC) {
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
    meth->get_access() & ACC_SYNTHETIC ? synth++ : others++;
  }
  TRACE(SYNT, 3, "synth methods %ld\n", synth);
  TRACE(SYNT, 3, "other methods %ld\n", others);
  return true;
}

bool optimize(const std::vector<DexClass*>& classes,
              const SynthConfig& synthConfig) {
  auto ssms = analyze(classes, synthConfig);
  assert(trace_analysis(ssms));
  transform(classes, ssms, synthConfig);
  return ssms.next_pass;
}

const int64_t MAX_PASSES = 5;
SynthConfig load_config(const folly::dynamic& config) {
  auto get_value = [&](const char* value, int64_t default_val) {
    if (config.isObject()) {
      auto it = config.find(value);
      if (it != config.items().end()) {
        TRACE(SYNT, 2, "Setting config %s to %ld\n", value,
              it->second.asInt());
        return it->second.asInt();
      }
    }
    TRACE(SYNT, 2, "For %s using default value %ld\n", value, default_val);
    return default_val;
  };

  int64_t max_passes = get_value("max_passes", MAX_PASSES);
  int64_t synth_only = get_value("synth_only", 0);
  int64_t remove_pub = get_value("remove_pub", 1);
  SynthConfig synthConfig = {max_passes, (bool)synth_only, (bool)remove_pub};
  return synthConfig;
}

void SynthPass::run_pass(DexClassesVector& dexen, PgoFiles& pgo) {
  SynthConfig synthConfig = load_config(m_config);
  Scope scope = build_class_scope(dexen);
  int passes = 0;
  do {
    TRACE(SYNT, 1, "Synth removal, pass %d\n", passes);
    bool more_opt_needed = optimize(scope, synthConfig);
    if (!more_opt_needed) break;
  } while (++passes < synthConfig.max_passes);
}
