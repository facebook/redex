/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace constant_propagation {

namespace intraprocedural {
class FixpointIterator;
} // namespace intraprocedural

namespace stringbuilder_append_chain {

/**
 * For every two-append `new StringBuilder().append(a).append(b).toString()`
 * sequence in `cfg` where a and b are provably non-null Strings, rewrite the
 * toString() into `a.concat(b)`. Both appends must be the String overload,
 * `append:(Ljava/lang/String;)Ljava/lang/StringBuilder;`; the other overloads
 * take a value that `concat:(Ljava/lang/String;)Ljava/lang/String;` cannot
 * accept. Non-nullness is what keeps the contents identical: `concat` throws
 * on a null receiver or argument, where `append` writes the text "null".
 *
 * `a + b` on Strings arrives as this shape from both Java and Kotlin, whose
 * `invokedynamic` to StringConcatFactory (since JVM target 9) D8 desugars into
 * the said pattern. Before:
 *
 *   new-instance v0, StringBuilder
 *   invoke-direct {v0} StringBuilder.<init>:()V
 *   invoke-virtual {v0, v1} StringBuilder.append:(String)StringBuilder
 *   invoke-virtual {v0, v2} StringBuilder.append:(String)StringBuilder
 *   invoke-virtual {v0} StringBuilder.toString:()String
 *   move-result-object v5
 *
 * After:
 *
 *   new-instance v0, StringBuilder
 *   invoke-direct {v0} StringBuilder.<init>:()V
 *   move-object v3, v1
 *   invoke-virtual {v0, v1} StringBuilder.append:(String)StringBuilder
 *   move-object v4, v2
 *   invoke-virtual {v0, v2} StringBuilder.append:(String)StringBuilder
 *   invoke-virtual {v3, v4} String.concat:(String)String
 *   move-result-object v5
 *
 * Non-nullness is read from `fp_iter`, an already-solved intraprocedural
 * constant-propagation fixpoint. When that fixpoint was seeded with an
 * interprocedural WholeProgramState (as inside IPConstantPropagation), operands
 * that are non-null across all callers -- e.g. a method parameter -- are proven
 * non-null too, which a purely intraprocedural run cannot see.
 *
 * The rewrite introduces a semantic change; see
 * `InterproceduralConstantPropagationPass::get_config_doc`.
 */
size_t reduce_two_append_concats(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg);

/*
 * Merge consecutive `append(String)` calls whose operands are all
 * compile-time constant Strings into a single `append` of the concatenation:
 * `...append("a").append("b")...` becomes `...append("ab")...`, saving one
 * invocation per merged append. It targets mixed builders (constant appends
 * interspersed with non-constant ones), since the source compilers already
 * fold a fully-constant concatenation into a single constant. Constant values
 * are read from `fp_iter`. Returns the number of appends eliminated. Before:
 *
 *   new-instance v0, StringBuilder
 *   invoke-direct {v0} StringBuilder.<init>:()V
 *   invoke-virtual {v0, v3} StringBuilder.append:(String)StringBuilder
 *   const-string v1, "a"
 *   invoke-virtual {v0, v1} StringBuilder.append:(String)StringBuilder
 *   const-string v1, "b"
 *   invoke-virtual {v0, v1} StringBuilder.append:(String)StringBuilder
 *
 * After:
 *
 *   new-instance v0, StringBuilder
 *   invoke-direct {v0} StringBuilder.<init>:()V
 *   invoke-virtual {v0, v3} StringBuilder.append:(String)StringBuilder
 *   const-string v1, "a"
 *   const-string v4, "ab"
 *   invoke-virtual {v0, v4} StringBuilder.append:(String)StringBuilder
 *   const-string v1, "b"
 *
 * A fresh register holds the concatenation, since an operand load may have
 * consumers elsewhere and so cannot be overwritten in place. The original
 * loads stay behind; where nothing else reads them, a later LocalDce run
 * removes them.
 *
 * Only appends threading a single builder register via the self-loop pattern
 * (R = R.append(...)) are merged, so dropping all but the first leaves that
 * register holding the builder for the following code.
 */
size_t merge_adjacent_constant_appends(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg);

} // namespace stringbuilder_append_chain

} // namespace constant_propagation
