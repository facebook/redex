/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Outliner.h"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "ReachableClasses.h"
#include "RedexResources.h"
#include "Trace.h"
#include "Walkers.h"
#include "Warning.h"

static Outliner s_pass;

namespace {

constexpr const char* DISPATCH_CLASS_NAME = "Lcom/facebook/redex/Outlined;";
constexpr const char* DISPATCH_METHOD_NAME = "$dispatch$throws";
using outlined_t = std::tuple<DexType*, DexString*>;
using namespace dex_asm;

DexMethodRef* get_ctor(DexType* type) {
  DexMethodRef* ctor = DexMethod::make_method(
      type,
      DexString::make_string("<init>"),
      DexProto::make_proto(get_void_type(),
                           DexTypeList::make_type_list({get_string_type()})));
  return ctor;
}

DexMethodRef* get_dispatch_method() {
  auto ex_type = DexType::get_type("Ljava/lang/Exception;");
  auto proto = DexProto::make_proto(
      ex_type, DexTypeList::make_type_list({get_int_type()}));
  auto target = DexType::make_type(DISPATCH_CLASS_NAME);
  return DexMethod::make_method(
      target, DexString::make_string(DISPATCH_METHOD_NAME), proto);
}

void build_dispatcher(DexStoresVector& stores,
                      const std::vector<outlined_t>& outlined_throws) {
  auto dispatch_method = get_dispatch_method();

  // make sure dispatcher doesn't already exist
  always_assert(!type_class(dispatch_method->get_class()));

  // prepare our outlined method creator
  MethodCreator* mc =
      new MethodCreator(dispatch_method->get_class(),
                        DexString::make_string(DISPATCH_METHOD_NAME),
                        dispatch_method->get_proto(),
                        ACC_PUBLIC | ACC_STATIC);

  // define args and locals
  auto outline_arg = mc->get_local(0);
  auto str_local = mc->make_local(get_string_type());
  auto ex_local = mc->make_local(DexType::get_type("Ljava/lang/Exception;"));

  // build up our outlined method
  auto mb = mc->get_main_block();
  std::map<int, MethodBlock*> cases;
  for (size_t idx = 0; idx < outlined_throws.size(); idx++) {
    cases[idx] = nullptr;
  }
  mb->load_null(ex_local);
  mb->switch_op(outline_arg, cases);
  for (auto case_entry : cases) {
    const outlined_t& outlined = outlined_throws.at(case_entry.first);
    DexType* type = std::get<0>(outlined);
    DexString* str = std::get<1>(outlined);
    always_assert(type);
    always_assert(str);
    TRACE(OUTLINE,
          1,
          "Outlined: %d %s %s\n",
          case_entry.first,
          SHOW(type),
          SHOW(str));
    auto case_block = case_entry.second;
    case_block->new_instance(type, ex_local);
    case_block->load_const(str_local, str);
    std::vector<Location> ctor_args{ex_local, str_local};
    case_block->invoke(OPCODE_INVOKE_DIRECT, get_ctor(type), ctor_args);
  }
  mb->throwex(ex_local);

  TRACE(OUTLINE, 1, "Method creator: %s\n", SHOW(mc));

  // create outline class and dispatch method
  auto dispatch_cls = new ClassCreator(dispatch_method->get_class());
  dispatch_cls->set_super(get_object_type());
  dispatch_cls->set_access(ACC_PUBLIC);
  dispatch_cls->add_method(mc->create());
  // add class to last dex of root store
  always_assert(!stores.empty());
  stores[0].get_dexen().rbegin()->emplace_back(dispatch_cls->create());
}

IRInstruction* make_invoke(const DexMethodRef* meth, uint16_t v0) {
  auto invoke = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke->set_method(const_cast<DexMethodRef*>(meth))
      ->set_arg_word_count(1)
      ->set_src(0, v0);
  return invoke;
}

/**
 * We only take classes from the root store, and we only take classes
 * in secondary dexes. (If there's only one dex in the root store, the
 * whole optimization will do nothing.)
 */
Scope build_scope(DexStoresVector& stores, bool include_primary_dex) {
  Scope v;
  always_assert(!stores.empty());
  const auto& dexen = stores[0].get_dexen();
  size_t offset = include_primary_dex ? 0 : 1;
  for (unsigned i = offset; i < dexen.size(); ++i) {
    for (auto cls : dexen[i]) {
      v.push_back(cls);
    }
  }
  return v;
}
}

void Outliner::run_pass(DexStoresVector& stores,
                        ConfigFiles& /* unused */,
                        PassManager& mgr) {
  auto scope = build_scope(stores, m_outline_primary_dex);

  DexMethodRef* dispatch_method = get_dispatch_method();

  // Outlining match pattern
  auto exception_type = DexType::get_type("Ljava/lang/Exception;");
  always_assert(exception_type);
  auto match = std::make_tuple(
      m::new_instance(m::opcode_type(m::is_assignable_to(exception_type))),
      m::move_result_pseudo(),
      m::const_string(),
      m::move_result_pseudo(),
      m::invoke_direct(m::opcode_method(m::can_be_constructor())),
      m::throwex());

  // Collect all throws we should outline
  std::vector<outlined_t> outlined_throws;
  walk::matching_opcodes_in_block(
      scope,
      match,
      [&](DexMethod* method,
          cfg::Block*,
          const std::vector<IRInstruction*>& insns) {
        always_assert(insns.size() == 6);

        auto new_instance = insns[0];
        auto new_instance_result = insns[1];
        auto const_string = insns[2];
        auto const_string_result = insns[3];
        auto invoke_direct = insns[4];
        IRInstruction* throwex = insns[5];
        if (invoke_direct->srcs_size() == 2 &&
            new_instance_result->dest() == invoke_direct->src(0) &&
            const_string_result->dest() == invoke_direct->src(1) &&
            new_instance_result->dest() == throwex->src(0)) {
          TRACE(OUTLINE,
                1,
                "Found pattern in %s:\n  %s\n  %s\n  %s\n  %s\n",
                SHOW(method),
                SHOW(new_instance),
                SHOW(const_string),
                SHOW(invoke_direct),
                SHOW(throwex));

          auto const_int_extype = dasm(OPCODE_CONST,
                                       {{VREG, new_instance_result->dest()},
                                        {LITERAL, outlined_throws.size()}});
          IRInstruction* invoke_static =
              make_invoke(dispatch_method, new_instance_result->dest());

          /*
              Nice code you got there. Be a shame if someone ever put an
	      infinite loop into it.

              (We have to emit a branch of some sort here to appease the
               verifier - all blocks either need to exit the method or
               jump somewhere)

              new-instance <TYPE> -> {vA}       => const-int {vA}, <EXTYPEORD>
              const-string <STRING> -> {vB}     => invoke-static <METHOD>,
              invoke-direct {vA}, {vB}, <CTTOR> => goto/32 +0 // will never run
              throw {vA}                        =>
          */
          outlined_t outlined{new_instance->get_type(),
                              const_string->get_string()};
          outlined_throws.emplace_back(outlined);

          IRCode* code = method->get_code();
          code->replace_opcode(new_instance, const_int_extype);
          code->replace_opcode(const_string, invoke_static);
          code->replace_opcode_with_infinite_loop(invoke_direct);
          code->remove_opcode(throwex);
        }
      });

  mgr.incr_metric("outlined_throws", outlined_throws.size());
  if (outlined_throws.size() > 0) {
    build_dispatcher(stores, outlined_throws);
  }
}
