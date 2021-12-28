/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Synth.h"

#include <memory>
#include <signal.h>
#include <stdio.h>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ApiLevelChecker.h"
#include "ClassHierarchy.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Mutators.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "RefChecker.h"
#include "Resolver.h"
#include "Show.h"
#include "SynthConfig.h"
#include "Walkers.h"

constexpr const char* METRIC_GETTERS_REMOVED = "getter_methods_removed_count";
constexpr const char* METRIC_WRAPPERS_REMOVED = "wrapper_methods_removed_count";
constexpr const char* METRIC_CTORS_REMOVED = "constructors_removed_count";
constexpr const char* METRIC_PASSES = "passes_count";
constexpr const char* METRIC_METHODS_STATICIZED = "methods_staticized_count";
constexpr const char* METRIC_PATCHED_INVOKES = "patched_invokes_count";
constexpr const char* METRIC_ILLEGAL_REFS = "illegal_refs";

namespace {
struct SynthMetrics {
  size_t getters_removed_count{0};
  size_t wrappers_removed_count{0};
  size_t ctors_removed_count{0};
  size_t methods_staticized_count{0};
  size_t patched_invokes_count{0};
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
  while (it != end && opcode::is_a_load_param(it->insn->opcode())) {
    ++it;
  }

  if (!opcode::is_an_iget(it->insn->opcode())) return nullptr;

  auto iget = it->insn;
  reg_t iget_dest = ir_list::move_result_pseudo_of(it.unwrap())->dest();
  std::advance(it, 2);

  if (!opcode::is_a_return_value(it->insn->opcode())) return nullptr;

  reg_t ret_reg = it->insn->src(0);
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
  while (it != end && opcode::is_a_load_param(it->insn->opcode())) {
    ++it;
  }

  if (!opcode::is_an_sget(it->insn->opcode())) return nullptr;

  auto sget = it->insn;
  reg_t sget_dest = ir_list::move_result_pseudo_of(it.unwrap())->dest();
  std::advance(it, 2);

  if (!opcode::is_a_return_value(it->insn->opcode())) return nullptr;

  reg_t ret_reg = it->insn->src(0);
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
  while (it != end && opcode::is_a_load_param(it->insn->opcode())) {
    ++it;
  }

  bool is_direct = it->insn->opcode() == OPCODE_INVOKE_DIRECT;
  bool is_static = it->insn->opcode() == OPCODE_INVOKE_STATIC;
  if (!is_direct && !is_static) return nullptr;

  auto invoke = it->insn;
  auto method = invoke->get_method();
  if (is_static) {
    method = resolve_static(type_class(method->get_class()), method->get_name(),
                            method->get_proto());
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
          "wrapper blocked:%s\nwrapped method:%s\nconflicts with:%s",
          SHOW(m),
          SHOW(method_def),
          SHOW(collision));
    return nullptr;
  }
  if (!passes_args_through(invoke, *code)) return nullptr;
  ++it;
  if (it == end) return nullptr;

  if (opcode::is_a_move_result(it->insn->opcode())) {
    ++it;
    if (it == end) return nullptr;
    if (!opcode::is_a_return_value(it->insn->opcode())) return nullptr;
    ++it;
    if (it != end) return nullptr; // exception handling code
  } else if (it->insn->opcode() == OPCODE_RETURN_VOID) {
    ++it;
    if (it != end) return nullptr; // exception handling code
  } else {
    return nullptr;
  }
  // The wrapper method may have a trivial exception handler.
  if (code->has_try_blocks()) return nullptr;
  return method_def;
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
  while (it != end && opcode::is_a_load_param(it->insn->opcode())) {
    ++it;
  }

  if (it->insn->opcode() != OPCODE_INVOKE_DIRECT) {
    TRACE(SYNT, 5, "Rejecting, not direct: %s", SHOW(m));
    return nullptr;
  }
  auto invoke = it->insn;
  if (!passes_args_through(invoke, *code, 1)) {
    TRACE(SYNT, 5, "Rejecting, not passthrough: %s", SHOW(m));
    return nullptr;
  }
  ++it;
  if (it == end) return nullptr;
  if (it->insn->opcode() != OPCODE_RETURN_VOID) return nullptr;
  auto method = invoke->get_method();
  if (!method->is_concrete() ||
      !method::is_constructor(static_cast<DexMethod*>(method))) {
    return nullptr;
  }
  return static_cast<DexMethod*>(method);
}

struct WrapperMethods {
  ConcurrentMap<DexMethod*, DexField*> getters;
  ConcurrentMap<DexMethod*, DexMethod*> wrappers;
  ConcurrentMap<DexMethod*, DexMethod*> ctors;
  ConcurrentMap<DexMethod*, std::pair<DexMethod*, int>> wrapped;
  ConcurrentSet<DexMethod*> keepers;
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
    if (ssms.wrappers.count_unsafe(p.second)) {
      remove.emplace_back(p.second);
    }
    if (ssms.getters.count_unsafe(p.second)) {
      // a getter is a leaf so we remove it and we'll likely pick
      // it up next pass
      TRACE(SYNT, 5, "Removing wrapped getter: %s", SHOW(p.second));
      ssms.getters.erase(p.second);
      ssms.next_pass = true;
    }
  }
  for (auto meth : remove) {
    auto wrapper = ssms.wrappers.find(meth);
    if (wrapper == ssms.wrappers.end()) {
      // Might have been a duplidate we already erased
      continue;
    }
    auto wrapped = ssms.wrapped.find(wrapper->second);
    if (wrapped != ssms.wrapped.end()) {
      if (--wrapped->second.second == 0) {
        TRACE(SYNT, 5, "Removing wrapped: %s", SHOW(wrapper->second));
        ssms.wrapped.erase(wrapper->second);
      }
    }
    TRACE(SYNT, 5, "Removing wrapper: %s", SHOW(meth));
    ssms.wrappers.erase(meth);
  }
  ssms.next_pass = ssms.next_pass || !remove.empty();
}

WrapperMethods analyze(const api::AndroidSDK* min_sdk_api,
                       XStoreRefs& xstores,
                       const ClassHierarchy& ch,
                       const std::vector<DexClass*>& classes,
                       const SynthConfig& synthConfig) {
  Timer timer("analyze");
  WrapperMethods ssms;
  walk::parallel::classes(classes, [&](DexClass* cls) {
    if (synthConfig.blocklist_types.count(cls->get_type())) {
      return;
    }
    for (auto dmethod : cls->get_dmethods()) {
      if (dmethod->rstate.dont_inline()) continue;

      // constructors are special and all we can remove are synthetic ones
      if (synthConfig.remove_constructors && is_synthetic(dmethod) &&
          method::is_constructor(dmethod)) {
        auto ctor = trivial_ctor_wrapper(dmethod);
        if (ctor) {
          TRACE(SYNT, 2, "Trivial constructor wrapper: %s", SHOW(dmethod));
          TRACE(SYNT, 2, "  Calls constructor: %s", SHOW(ctor));
          ssms.ctors.emplace(dmethod, ctor);
        }
        continue;
      }
      if (method::is_constructor(dmethod)) continue;

      if (is_static_synthetic(dmethod)) {
        auto field = trivial_get_field_wrapper(dmethod);
        if (field) {
          TRACE(SYNT, 2, "Static trivial getter: %s", SHOW(dmethod));
          TRACE(SYNT, 2, "  Gets field: %s", SHOW(field));
          ssms.getters.emplace(dmethod, field);
          continue;
        }
        auto sfield = trivial_get_static_field_wrapper(dmethod);
        if (sfield) {
          TRACE(SYNT, 2, "Static trivial static field getter: %s",
                SHOW(dmethod));
          TRACE(SYNT, 2, "  Gets static field: %s", SHOW(sfield));
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
          TRACE(SYNT, 2, "Static trivial method wrapper: %s", SHOW(dmethod));
          TRACE(SYNT, 2, "  Calls method: %s", SHOW(method));
          ssms.wrappers.emplace(dmethod, method);
          if (!is_static(method)) {
            ssms.wrapped.update(
                method,
                [&](DexMethod*, std::pair<DexMethod*, int>& p, bool exists) {
                  if (!exists) {
                    p = std::make_pair(dmethod, 1);
                  } else {
                    p.second++;
                  }
                });
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
  });
  purge_wrapped_wrappers(ssms);
  return ssms;
}

IRInstruction* make_iget(DexField* field, reg_t src) {
  auto const opcode = [&]() {
    switch (type::to_datatype(field->get_type())) {
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
      not_reached();
    }
    not_reached();
  }();

  return (new IRInstruction(opcode))->set_field(field)->set_src(0, src);
}

IRInstruction* make_sget(DexField* field) {
  auto const opcode = opcode::sget_opcode_for_field(field);
  return (new IRInstruction(opcode))->set_field(field);
}

void replace_getter_wrapper_sequential(IRInstruction* insn, DexField* field) {
  TRACE(SYNT, 2, "Optimizing getter wrapper call (sequential): %s", SHOW(insn));
  redex_assert(field->is_concrete());
  set_public(field);
  always_assert(is_public(field));
}

void replace_getter_wrapper_concurrent(IRCode* transform,
                                       IRInstruction* insn,
                                       IRInstruction* move_result,
                                       DexField* field) {
  TRACE(SYNT, 2, "Optimizing getter wrapper call (concurrent): %s", SHOW(insn));
  redex_assert(field->is_concrete());
  always_assert(is_public(field));

  auto new_get =
      is_static(field) ? make_sget(field) : make_iget(field, insn->src(0));
  TRACE(SYNT, 2, "Created instruction: %s", SHOW(new_get));
  auto move_result_pseudo =
      (new IRInstruction(opcode::move_result_to_pseudo(move_result->opcode())))
          ->set_dest(move_result->dest());

  transform->replace_opcode(insn, {new_get, move_result_pseudo});
  transform->remove_opcode(move_result);
}

void replace_method_wrapper_concurrent(IRCode* transform,
                                       IRInstruction* insn,
                                       DexMethod* method) {
  TRACE(SYNT, 2, "Optimizing method wrapper (sequential): %s", SHOW(insn));
  auto op = insn->opcode();
  auto new_invoke = [&] {
    redex_assert(op == OPCODE_INVOKE_STATIC || op == OPCODE_INVOKE_DIRECT);
    auto new_op =
        is_static(method) ? OPCODE_INVOKE_STATIC : OPCODE_INVOKE_DIRECT;
    auto ret = new IRInstruction(new_op);
    ret->set_method(method)->set_srcs_size(insn->srcs_size());
    for (size_t i = 0; i < ret->srcs_size(); i++) {
      ret->set_src(i, insn->src(i));
    }
    return ret;
  }();

  TRACE(SYNT, 2, "new instruction: %s", SHOW(new_invoke));
  transform->replace_opcode(insn, new_invoke);
}

bool can_update_wrappee(const ClassHierarchy& ch,
                        DexMethod* wrappee,
                        DexMethod* wrapper) {
  if (is_native(wrappee) || !can_rename(wrappee)) {
    // Can't change the signature of native methods, as well as
    // unrenameable ones.
    return false;
  }
  DexProto* old_proto = wrappee->get_proto();
  auto new_args = old_proto->get_args()->push_front(wrappee->get_class());
  DexProto* new_proto = DexProto::make_proto(old_proto->get_rtype(), new_args);
  auto new_name = wrappee->get_name();
  auto new_class = type_class(wrappee->get_class());
  if (find_collision(ch, new_name, new_proto, new_class, false)) {
    if (find_collision_excepting(ch,
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

void replace_method_wrapper_sequential(const ClassHierarchy& ch,
                                       IRInstruction* insn,
                                       DexMethod* wrapper,
                                       DexMethod* wrappee,
                                       WrapperMethods& ssms) {
  TRACE(SYNT, 2, "Optimizing method wrapper (sequential): %s", SHOW(insn));
  TRACE(SYNT, 3, "  wrapper:%p wrappee:%p", wrapper, wrappee);
  TRACE(SYNT, 3, "  wrapper: %s", SHOW(wrapper));
  TRACE(SYNT, 3, "  wrappee: %s", SHOW(wrappee));
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
}

void replace_ctor_wrapper_sequential(IRInstruction* ctor_insn,
                                     DexMethod* ctor) {
  TRACE(SYNT, 2, "Optimizing static ctor (sequential): %s", SHOW(ctor_insn));
  redex_assert(ctor->is_concrete());
  set_public(ctor);
  always_assert(is_public(ctor));
}

void replace_ctor_wrapper_concurrent(IRCode* transform,
                                     IRInstruction* ctor_insn,
                                     DexMethod* ctor) {
  TRACE(SYNT, 2, "Optimizing static ctor (concurrent): %s", SHOW(ctor_insn));
  redex_assert(ctor->is_concrete());
  always_assert(is_public(ctor));

  auto op = ctor_insn->opcode();
  auto new_ctor_call = [&] {
    redex_assert(op == OPCODE_INVOKE_DIRECT);
    auto ret = new IRInstruction(OPCODE_INVOKE_DIRECT);
    ret->set_method(ctor)->set_srcs_size(ctor_insn->srcs_size() - 1);
    for (size_t i = 0; i < ret->srcs_size(); i++) {
      ret->set_src(i, ctor_insn->src(i));
    }
    return ret;
  }();

  TRACE(SYNT, 2, "new instruction: %s", SHOW(new_ctor_call));
  transform->replace_opcode(ctor_insn, new_ctor_call);
}

struct MethodAnalysisResult {
  std::vector<std::tuple<IRInstruction*, IRInstruction*, DexField*>>
      getter_calls;
  std::vector<std::tuple<IRInstruction*, DexMethod*, DexMethod*>> wrapper_calls;
  std::vector<std::tuple<IRInstruction*, DexMethod*, DexMethod*>> wrapped_calls;
  std::vector<std::pair<IRInstruction*, DexMethod*>> ctor_calls;
};

std::unique_ptr<MethodAnalysisResult> analyze_method_concurrent(
    RefChecker& ref_checker,
    DexMethod* caller_method,
    WrapperMethods& ssms,
    std::atomic<size_t>& illegal_refs) {
  auto mar = std::make_unique<MethodAnalysisResult>();
  TRACE(SYNT, 4, "Analyzing %s", SHOW(caller_method));
  auto ii = InstructionIterable(caller_method->get_code());
  int32_t caller_api_level = api::LevelChecker::get_method_level(caller_method);
  auto check_callee = [&](DexMethod* callee) {
    int32_t callee_api_level = api::LevelChecker::get_method_level(callee);
    if (callee_api_level != api::LevelChecker::get_min_level() &&
        callee_api_level > caller_api_level) {
      return false;
    }
    if (ref_checker.check_method(callee)) {
      return true;
    }
    illegal_refs++;
    return false;
  };
  auto check_field = [&](DexField* field) {
    if (ref_checker.check_field(field)) {
      return true;
    }
    illegal_refs++;
    return false;
  };
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
        auto field = found_get->second;
        if (!opcode::is_a_move_result(move_result->opcode()) ||
            !check_field(field)) {
          ssms.keepers.emplace(callee);
          continue;
        }
        mar->getter_calls.emplace_back(insn, move_result, field);
        continue;
      }

      auto const found_wrap = ssms.wrappers.find(callee);
      if (found_wrap != ssms.wrappers.end()) {
        auto method = found_wrap->second;
        if (check_callee(method)) {
          mar->wrapper_calls.emplace_back(insn, callee, method);
        } else {
          ssms.keepers.emplace(callee);
          ssms.keepers.emplace(method);
        }
        continue;
      }
      always_assert_log(ssms.wrapped.find(callee) == ssms.wrapped.end(),
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
        auto field = found_get->second;
        if (!opcode::is_a_move_result(move_result->opcode()) ||
            !check_field(field)) {
          ssms.keepers.emplace(callee);
          continue;
        }
        mar->getter_calls.emplace_back(insn, move_result, field);
        continue;
      }

      auto const found_wrap = ssms.wrappers.find(callee);
      if (found_wrap != ssms.wrappers.end()) {
        auto method = found_wrap->second;
        if (check_callee(method)) {
          mar->wrapper_calls.emplace_back(insn, callee, method);
        } else {
          ssms.keepers.emplace(callee);
          ssms.keepers.emplace(method);
        }
        continue;
      }

      auto const found_wrappee = ssms.wrapped.find(callee);
      if (found_wrappee != ssms.wrapped.end()) {
        auto wrapper = found_wrappee->second.first;
        if (check_callee(wrapper)) {
          mar->wrapped_calls.emplace_back(insn, callee, wrapper);
        } else {
          ssms.keepers.emplace(callee);
          ssms.keepers.emplace(wrapper);
        }
        continue;
      }

      auto const found_ctor = ssms.ctors.find(callee);
      if (found_ctor != ssms.ctors.end()) {
        auto ctor = found_ctor->second;
        if (check_callee(ctor)) {
          mar->ctor_calls.emplace_back(insn, ctor);
        } else {
          ssms.keepers.emplace(callee);
          ssms.keepers.emplace(ctor);
        }
        continue;
      }
    }
  }
  return mar;
}

void replace_wrappers_sequential(const ClassHierarchy& ch,
                                 DexMethod* caller_method,
                                 WrapperMethods& ssms,
                                 MethodAnalysisResult* mar) {
  using std::get;
  TRACE(SYNT, 4, "Replacing wrappers (sequential) in %s", SHOW(caller_method));
  // Prune out wrappers that are invalid due to naming conflicts.
  std::unordered_set<DexMethod*> bad_wrappees;
  std::unordered_multimap<DexMethod*, DexMethod*> wrappees_to_wrappers;
  for (const auto& wtriple : mar->wrapper_calls) {
    auto call_inst = get<0>(wtriple);
    auto wrapper = static_cast<DexMethod*>(call_inst->get_method());
    auto wrappee = get<2>(wtriple);
    wrappees_to_wrappers.emplace(wrappee, wrapper);
    if (!can_update_wrappee(ch, wrappee, wrapper)) {
      bad_wrappees.emplace(wrappee);
    }
  }
  for (const auto& wtriple : mar->wrapped_calls) {
    auto call_inst = get<0>(wtriple);
    auto wrapper = get<2>(wtriple);
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
  mar->wrapper_calls.erase(
      std::remove_if(
          mar->wrapper_calls.begin(),
          mar->wrapper_calls.end(),
          [&](const std::tuple<IRInstruction*, DexMethod*, DexMethod*>&
                  wtriple) { return bad_wrappees.count(get<2>(wtriple)); }),
      mar->wrapper_calls.end());
  mar->wrapped_calls.erase(
      std::remove_if(mar->wrapped_calls.begin(),
                     mar->wrapped_calls.end(),
                     [&](const std::tuple<IRInstruction*, DexMethod*,
                                          DexMethod*>& wtriple) {
                       auto call_inst = get<0>(wtriple);
                       return bad_wrappees.count(
                           static_cast<DexMethod*>(call_inst->get_method()));
                     }),
      mar->wrapped_calls.end());
  // Fix up everything left.
  for (const auto& g : mar->getter_calls) {
    replace_getter_wrapper_sequential(get<0>(g), get<2>(g));
  }
  for (const auto& wtriple : mar->wrapper_calls) {
    auto call_inst = get<0>(wtriple);
    auto wrapper = get<1>(wtriple);
    auto wrappee = get<2>(wtriple);
    replace_method_wrapper_sequential(ch, call_inst, wrapper, wrappee, ssms);
  }
  for (const auto& wtriple : mar->wrapped_calls) {
    auto call_inst = get<0>(wtriple);
    auto wrappee = get<1>(wtriple);
    auto wrapper = get<2>(wtriple);
    replace_method_wrapper_sequential(ch, call_inst, wrapper, wrappee, ssms);
  }
  for (const auto& cpair : mar->ctor_calls) {
    replace_ctor_wrapper_sequential(cpair.first, cpair.second);
  }
}

void replace_wrappers_concurrent(DexMethod* caller_method,
                                 const MethodAnalysisResult* mar) {
  using std::get;
  auto code = caller_method->get_code();
  for (const auto& g : mar->getter_calls) {
    replace_getter_wrapper_concurrent(code, get<0>(g), get<1>(g), get<2>(g));
  }
  for (const auto& wtriple : mar->wrapper_calls) {
    auto call_inst = get<0>(wtriple);
    auto wrappee = get<2>(wtriple);
    replace_method_wrapper_concurrent(code, call_inst, wrappee);
  }
  for (const auto& wtriple : mar->wrapped_calls) {
    auto call_inst = get<0>(wtriple);
    auto wrappee = get<1>(wtriple);
    replace_method_wrapper_concurrent(code, call_inst, wrappee);
  }
  for (const auto& cpair : mar->ctor_calls) {
    replace_ctor_wrapper_concurrent(code, cpair.first, cpair.second);
  }
}

void remove_dead_methods(WrapperMethods& ssms,
                         const SynthConfig& synthConfig,
                         SynthMetrics& metrics) {
  bool any_remove = false;
  size_t synth_removed = 0;
  size_t other_removed = 0;
  size_t pub_meth = 0;
  std::unordered_map<DexClass*, std::unordered_set<DexMethod*>>
      methods_to_remove_by_class;
  auto remove_meth = [&](DexMethod* meth) {
    redex_assert(meth->is_concrete());
    if (!can_remove(meth, synthConfig)) {
      return;
    }
    if (ssms.keepers.count(meth)) {
      TRACE(SYNT, 2, "Retaining method: %s", SHOW(meth));
      return;
    }
    if (!can_delete(meth)) {
      TRACE(SYNT, 2, "Do not strip: %s", SHOW(meth));
      return;
    }

    TRACE(SYNT, 2, "Removing method: %s", SHOW(meth));
    if (is_public(meth)) pub_meth++;
    auto cls = type_class(meth->get_class());
    methods_to_remove_by_class[cls].insert(meth);
    is_synthetic(meth) ? synth_removed++ : other_removed++;
  };

  for (auto const& gp : ssms.getters) {
    remove_meth(gp.first);
  }
  any_remove = any_remove || (synth_removed && other_removed);
  TRACE(SYNT, 3, "any_remove = %d", any_remove);
  TRACE(SYNT, 3, "synth_removed = %zu", synth_removed);
  TRACE(SYNT, 3, "other_removed = %zu", other_removed);
  if (synth_removed) {
    TRACE(SYNT, 1, "Synthetic getters removed %ld", synth_removed);
  }
  if (other_removed) {
    TRACE(SYNT, 1, "Other getters removed %ld", other_removed);
  }
  if (pub_meth) {
    TRACE(SYNT, 1, "Public getters removed %ld", pub_meth);
  }

  metrics.getters_removed_count += synth_removed + other_removed;

  synth_removed = 0;
  other_removed = 0;
  pub_meth = 0;
  for (auto const& wp : ssms.wrappers) {
    remove_meth(wp.first);
  }
  any_remove = any_remove || (synth_removed && other_removed);
  if (synth_removed) {
    TRACE(SYNT, 1, "Synthetic wrappers removed %ld", synth_removed);
  }
  if (other_removed) {
    TRACE(SYNT, 1, "Other wrappers removed %ld", other_removed);
  }
  if (pub_meth) {
    TRACE(SYNT, 1, "Public wrappers removed %ld", pub_meth);
  }

  metrics.wrappers_removed_count += synth_removed + other_removed;

  synth_removed = 0;
  other_removed = 0;
  pub_meth = 0;
  for (auto const& ct : ssms.ctors) {
    remove_meth(ct.first);
  }
  any_remove = any_remove || (synth_removed && other_removed);
  if (synth_removed) {
    TRACE(SYNT, 1, "Synthetic constructor removed %ld", synth_removed);
  }
  if (pub_meth) {
    TRACE(SYNT, 1, "Public constructor removed %ld", pub_meth);
  }

  metrics.ctors_removed_count += synth_removed;

  redex_assert(other_removed == 0);
  ssms.next_pass = ssms.next_pass && any_remove;

  std::vector<DexClass*> classes;
  classes.reserve(methods_to_remove_by_class.size());
  for (auto& p : methods_to_remove_by_class) {
    classes.push_back(p.first);
  }
  walk::parallel::classes(classes, [&](DexClass* clazz) {
    for (auto m : methods_to_remove_by_class.at(clazz)) {
      clazz->remove_method(m);
    }
  });
}

void do_transform(const api::AndroidSDK* min_sdk_api,
                  XStoreRefs& xstores,
                  const ClassHierarchy& ch,
                  const std::vector<DexClass*>& classes,
                  WrapperMethods& ssms,
                  const SynthConfig& synthConfig,
                  SynthMetrics& metrics,
                  std::atomic<size_t>& illegal_refs) {
  Timer timer("do_transform");
  // remove wrappers.  build a vector ahead of time to ensure we only visit each
  // method once, even if we mutate the class method lists such that we'd hit
  // something a second time.
  std::vector<DexMethod*> methods;
  std::unordered_map<DexMethod*, std::unique_ptr<MethodAnalysisResult>>
      method_analysis_results;
  walk::code(classes, [&](DexMethod* meth, IRCode&) {
    methods.emplace_back(meth);
    method_analysis_results.emplace(meth,
                                    std::unique_ptr<MethodAnalysisResult>());
  });

  std::vector<std::unique_ptr<RefChecker>> ref_checkers;
  ref_checkers.reserve(xstores.size());
  for (size_t store_idx = 0; store_idx < xstores.size(); store_idx++) {
    ref_checkers.emplace_back(
        std::make_unique<RefChecker>(&xstores, store_idx, min_sdk_api));
  }
  // Analyze methods in parallel (no mutation)
  walk::parallel::code(classes, [&](DexMethod* meth, IRCode&) {
    auto store_idx = xstores.get_store_idx(meth->get_class());
    auto& ref_checker = ref_checkers.at(store_idx);
    method_analysis_results.at(meth) =
        analyze_method_concurrent(*ref_checker, meth, ssms, illegal_refs);
  });

  // Mutate method signatures (sequentially, as they are subtle dependencies)
  for (auto const& meth : methods) {
    auto* mar = method_analysis_results.at(meth).get();
    replace_wrappers_sequential(ch, meth, ssms, mar);
  }

  // Mutate method bodies (concurrently), and check that invokes to promoted
  // static method are correct
  std::atomic<size_t> patched_invokes{0};
  walk::parallel::code(classes, [&](DexMethod* meth, IRCode& code) {
    auto* mar = method_analysis_results.at(meth).get();
    replace_wrappers_concurrent(meth, mar);
    for (auto& mie : InstructionIterable(code)) {
      auto* insn = mie.insn;
      auto opcode = insn->opcode();
      if (opcode != OPCODE_INVOKE_DIRECT) {
        continue;
      }
      auto wrappee = resolve_method(insn->get_method(), MethodSearch::Direct);
      if (wrappee == nullptr || ssms.promoted_to_static.count(wrappee) == 0) {
        continue;
      }
      // change the opcode to invoke-static
      insn->set_opcode(OPCODE_INVOKE_STATIC);
      TRACE(SYNT, 3, "Updated invoke on promoted to static %s\n in method %s",
            SHOW(wrappee), SHOW(meth));
      patched_invokes++;
    }
  });
  remove_dead_methods(ssms, synthConfig, metrics);
  metrics.methods_staticized_count += ssms.promoted_to_static.size();
  metrics.patched_invokes_count += (size_t)patched_invokes;
}

bool trace_analysis(WrapperMethods& ssms) {
  DEBUG_ONLY size_t synth = 0;
  DEBUG_ONLY size_t others = 0;
  for (auto it : ssms.getters) {
    auto meth = it.first;
    is_synthetic(meth) ? synth++ : others++;
  }
  TRACE(SYNT, 3, "synth getters %ld", synth);
  TRACE(SYNT, 3, "other getters %ld", others);

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
  TRACE(SYNT, 3, "synth ctors %ld", synth);
  TRACE(SYNT, 3, "other ctors %ld", others);

  synth = 0;
  others = 0;
  for (auto it : ssms.wrappers) {
    auto meth = it.first;
    is_synthetic(meth) ? synth++ : others++;
  }
  TRACE(SYNT, 3, "synth methods %ld", synth);
  TRACE(SYNT, 3, "other methods %ld", others);
  return true;
}

bool optimize(const api::AndroidSDK* min_sdk_api,
              XStoreRefs& xstores,
              const ClassHierarchy& ch,
              const std::vector<DexClass*>& classes,
              const SynthConfig& synthConfig,
              SynthMetrics& metrics,
              std::atomic<size_t>& illegal_refs) {
  auto ssms = analyze(min_sdk_api, xstores, ch, classes, synthConfig);
  redex_assert(trace_analysis(ssms));
  do_transform(min_sdk_api, xstores, ch, classes, ssms, synthConfig, metrics,
               illegal_refs);
  return ssms.next_pass;
}

void SynthPass::run_pass(DexStoresVector& stores,
                         ConfigFiles& conf,
                         PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(SYNT, 1,
          "SynthPass not run because no ProGuard configuration was provided.");
    return;
  }

  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  mgr.incr_metric("min_sdk", min_sdk);
  TRACE(SYNT, 2, "min_sdk: %d", min_sdk);
  auto min_sdk_api_file = conf.get_android_sdk_api_file(min_sdk);
  const api::AndroidSDK* min_sdk_api{nullptr};
  if (!min_sdk_api_file) {
    mgr.incr_metric("min_sdk_no_file", 1);
    TRACE(SYNT, 2, "Android SDK API %d file cannot be found.", min_sdk);
  } else {
    min_sdk_api = &conf.get_android_sdk_api(min_sdk);
  }

  XStoreRefs xstores(stores);
  Scope scope = build_class_scope(stores);
  ClassHierarchy ch = build_type_hierarchy(scope);
  SynthMetrics metrics;
  int passes = 0;
  std::atomic<size_t> illegal_refs{0};
  do {
    TRACE(SYNT, 1, "Synth removal, pass %d", passes);
    bool more_opt_needed = optimize(min_sdk_api, xstores, ch, scope,
                                    m_pass_config, metrics, illegal_refs);
    if (!more_opt_needed) break;
  } while (++passes < m_pass_config.max_passes);

  mgr.incr_metric(METRIC_GETTERS_REMOVED, metrics.getters_removed_count);
  mgr.incr_metric(METRIC_WRAPPERS_REMOVED, metrics.wrappers_removed_count);
  mgr.incr_metric(METRIC_CTORS_REMOVED, metrics.ctors_removed_count);
  mgr.incr_metric(METRIC_METHODS_STATICIZED, metrics.methods_staticized_count);
  mgr.incr_metric(METRIC_PATCHED_INVOKES, metrics.patched_invokes_count);
  mgr.incr_metric(METRIC_ILLEGAL_REFS, illegal_refs);
  mgr.incr_metric(METRIC_PASSES, passes);
}

static SynthPass s_pass;
