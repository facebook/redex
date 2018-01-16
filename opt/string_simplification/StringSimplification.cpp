/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "StringSimplification.h"
#include "DexClass.h"
#include "StringDomain.h"
#include "StringIterator.h"
#include "Walkers.h"

constexpr const char* NUM_CONST_STRINGS_ADDED = "num_const_strings_added";
constexpr const char* NUM_INSTRUCTIONS_ADDED = "num_instructions_added";
constexpr const char* NUM_INSTRUCTIONS_REMOVED = "num_instructions_removed";

void StringSimplificationPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* cfg */,
                                        PassManager& mgr) {
  auto scope = build_class_scope(stores);
  walk::code(scope, [&](DexMethod* m, IRCode& code) {
    TRACE(STR_SIMPLE, 8, "Method: %s\n", SHOW(m));
    code.build_cfg();
    StringIterator iter(&code, code.cfg().entry_block());
    iter.run(StringProdEnvironment());
    iter.simplify();
    mgr.incr_metric(NUM_CONST_STRINGS_ADDED, iter.get_strings_added());
    mgr.incr_metric(NUM_INSTRUCTIONS_ADDED, iter.get_instructions_added());
    mgr.incr_metric(NUM_INSTRUCTIONS_REMOVED, iter.get_instructions_removed());
  });
}

static StringSimplificationPass s_pass;
