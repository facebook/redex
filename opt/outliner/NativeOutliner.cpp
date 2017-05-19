/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "NativeOutliner.h"
#include "NativeOutliner_generated.h"

#include <algorithm>
#include <map>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "RedexResources.h"
#include "Trace.h"
#include "Transform.h"
#include "Walkers.h"
#include "Warning.h"

static NativeOutliner s_pass;

namespace {

constexpr const char* THROWABLE_TYPE_NAME = "Ljava/lang/Throwable;";
constexpr const char* DISPATCH_CLASS_NAME = "Lcom/facebook/redex/NativeOutlined;";
constexpr const char* DISPATCH_METHOD_NAME = "$dispatch$throws";

using outlined_t = std::tuple<DexType*, DexString*>;
using namespace dex_asm;
using namespace facebook::redex;

DexMethod* get_dispatch_method() {
  auto throwable_type = DexType::get_type(THROWABLE_TYPE_NAME);
  auto proto = DexProto::make_proto(
      throwable_type, DexTypeList::make_type_list({get_int_type()}));
  auto target = DexType::make_type(DISPATCH_CLASS_NAME);
  return DexMethod::make_method(
      target, DexString::make_string(DISPATCH_METHOD_NAME), proto);
}

IRInstruction* make_invoke(const DexMethod* meth, uint16_t v0) {
  auto invoke = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke->set_method(const_cast<DexMethod*>(meth));
  invoke->set_arg_word_count(1);
  invoke->set_src(0, v0);
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

void NativeOutliner::run_pass(DexStoresVector& stores,
                        ConfigFiles& cfg,
                        PassManager& mgr) {
  std::string artifacts_filename = cfg.metafile(m_artifacts_filename);
  FILE* fd = fopen(artifacts_filename.c_str(), "wb");
  if (fd == nullptr) {
    perror(artifacts_filename.c_str());
    exit(EXIT_FAILURE);
  }

  auto scope = build_scope(stores, m_outline_primary_dex);

  DexMethod* dispatch_method = get_dispatch_method();

  // Outlining match pattern
  auto throwable_type = DexType::get_type(THROWABLE_TYPE_NAME);
  always_assert(throwable_type);
  auto match = std::make_tuple(
      m::new_instance(m::opcode_type(m::is_assignable_to(throwable_type))),
      m::const_string(),
      m::invoke_direct(m::opcode_method(m::is_constructor())),
      m::throwex());

  // Collect all throws we should outline
  std::vector<outlined_t> outlined_throws;
  walk_matching_opcodes_in_block(
      scope,
      match,
      [&](const DexMethod* method,
          IRCode* mt,
          Block* bb,
          size_t n,
          IRInstruction** insns) {
        always_assert(n == 4);
        std::string type_name(method->get_class()->get_name()->c_str());
        if (std::find(m_dont_outline_types.begin(), m_dont_outline_types.end(), type_name) != m_dont_outline_types.end()) {
          return;
        }

        auto new_instance = insns[0];
        auto const_string = insns[1];
        auto invoke_direct = insns[2];
        IRInstruction* throwex = insns[3];
        if (invoke_direct->srcs_size() == 2 &&
            new_instance->dest() == invoke_direct->src(0) &&
            const_string->dest() == invoke_direct->src(1) &&
            new_instance->dest() == throwex->src(0)) {
          TRACE(OUTLINE,
                2,
                "Found pattern in %s (%p):\n  %s\n  %s\n  %s\n  %s\n",
                SHOW(method),
                bb,
                SHOW(new_instance),
                SHOW(const_string),
                SHOW(invoke_direct),
                SHOW(throwex));

          auto const_int_extype = dasm(OPCODE_CONST,
                                       {{VREG, new_instance->dest()},
                                        {LITERAL, outlined_throws.size()}});
          IRInstruction* invoke_static =
              make_invoke(dispatch_method, new_instance->dest());

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
          outlined_throws.emplace_back(
            new_instance->get_type(), const_string->get_string());

          mt->replace_opcode(new_instance, const_int_extype);
          mt->replace_opcode(const_string, invoke_static);
          mt->replace_opcode_with_infinite_loop(invoke_direct);
          mt->remove_opcode(throwex);
        }
      });

  mgr.incr_metric("outlined_throws", outlined_throws.size());

  flatbuffers::FlatBufferBuilder fbb(1024);
  std::vector<flatbuffers::Offset<OutlinedThrow>> outlined_throw_vec;
  for (size_t i = 0 ; i < outlined_throws.size() ; ++i) {
    auto out = outlined_throws[i];
    auto type_loc = fbb.CreateString(std::get<0>(out)->get_name()->c_str());
    auto msg_loc = fbb.CreateString(std::get<1>(out)->c_str());
    auto outlined_throw_loc = CreateOutlinedThrow(fbb, i, type_loc, msg_loc);
    outlined_throw_vec.emplace_back(outlined_throw_loc);
  }
  TRACE(OUTLINE, 1, "Native outlined %zu throws\n", outlined_throw_vec.size());
  auto outlined_throws_loc = CreateOutlinedThrows(fbb, fbb.CreateVector(outlined_throw_vec));
  fbb.Finish(outlined_throws_loc);
  uint8_t *buf = fbb.GetBufferPointer();
  int size = fbb.GetSize();
  fwrite(buf, size, 1, fd);
  fclose(fd);
}
