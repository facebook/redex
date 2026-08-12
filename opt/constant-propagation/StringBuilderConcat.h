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

namespace stringbuilder_concat {

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
 * ## Semantic Change
 *
 * There is a semantics change, in reference identity only. Where b is empty,
 * `concat` is documented to return its receiver, so a concatenation that could
 * only have produced a new object now produces a itself:
 *
 *   String a = readName();       // "foo", not a compile-time constant
 *   String s = a + "";
 *   s.equals(a);                 // true, before and after
 *   s == a;                      // before: false. after: true.
 *
 * The pre-reduction `false` is guaranteed rather than incidental: JLS 15.18.1
 * specifies the result of `+` to be a newly created String unless the
 * expression is constant.
 *
 * Only identity-sensitive code can observe the change: `==`,
 * `identityHashCode`, `IdentityHashMap`, a monitor held on the result, or a
 * weak reference to it.
 */
size_t reduce_two_append_concats(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg);

} // namespace stringbuilder_concat

} // namespace constant_propagation
