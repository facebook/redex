/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"

using MethodOrderedSet = std::set<DexMethod*, dexmethods_comparator>;

namespace method_reference {

// Caller to invoke instruction
using CallSites = std::vector<std::pair<DexMethod*, IRInstruction*>>;

struct CallSiteSpec {
  DexMethod* caller;
  IRInstruction* call_insn;
  DexMethod* new_callee;
};

IRInstruction* make_load_const(uint16_t dest, size_t val);

IRInstruction* make_invoke(DexMethod* callee,
                           IROpcode opcode,
                           std::vector<uint16_t> args);

/**
 * An optional list of additional arguments that we can pass to the patched call
 * site.
 */
void patch_callsite_var_additional_args(
    const CallSiteSpec& spec,
    const boost::optional<std::vector<uint32_t>>& additional_args =
        boost::none);

/**
 * An optional additional argument that we want to pass to the patched call
 * site. One example would be passing the type tag in type erased code.
 */
void patch_callsite(
    const CallSiteSpec& spec,
    const boost::optional<uint32_t>& additional_arg = boost::none);

void update_call_refs_simple(
    const Scope& scope,
    const std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee);

CallSites collect_call_refs(const Scope& scope,
                            const MethodOrderedSet& callees);
}
