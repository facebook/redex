/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <functional>
#include <vector>
#include "ControlFlow.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "Match.h"
#include "Transform.h"

/**
 * Walk all methods of all classes defined in 'scope' calling back
 * the walker function.
 */
template <class T, class MethodWalkerFn = void(DexMethod*)>
void walk_methods(const T& scope, MethodWalkerFn walker) {
  for (const auto& cls : scope) {
    for (auto dmethod : cls->get_dmethods()) {
      TraceContext context(dmethod->get_deobfuscated_name());
      walker(dmethod);
    }
    for (auto vmethod : cls->get_vmethods()) {
      TraceContext context(vmethod->get_deobfuscated_name());
      walker(vmethod);
    }
  };
}

/**
 * Walk all fields of all classes defined in 'scope' calling back
 * the walker function.
 */
template <class T, class FieldWalkerFn = void(DexField*)>
void walk_fields(const T& scope, FieldWalkerFn walker) {
  for (const auto& cls : scope) {
    for (auto ifield : cls->get_ifields()) {
      walker(ifield);
    }
    for (auto sfield : cls->get_sfields()) {
      walker(sfield);
    }
  };
}

/**
 * Walk the code of every method that satisfies the filter function, for all
 * classes defined in 'scope' calling back the walker function.
 */
template <class T,
          class MethodFilterFn = bool(DexMethod*),
          class CodeWalkerFn = void(DexMethod*, DexCode&)>
void walk_code(const T& scope,
               MethodFilterFn methodFilter,
               CodeWalkerFn codeWalker) {
  for (const auto& cls : scope) {
    for (auto dmethod : cls->get_dmethods()) {
      if (methodFilter(dmethod)) {
        auto& code = dmethod->get_code();
        if (code) codeWalker(dmethod, *code);
      }
    }
    for (auto vmethod : cls->get_vmethods()) {
      if (methodFilter(vmethod)) {
        auto& code = vmethod->get_code();
        if (code) codeWalker(vmethod, *code);
      }
    }
  };
}

template <class T,
          class MethodFilterFn = bool(DexMethod*),
          class InstructionWalkerFn = void(DexMethod*, DexInstruction*)>
void walk_opcodes(const T& scope,
                  MethodFilterFn methodFilter,
                  InstructionWalkerFn opcodeWalker) {
  for (const auto& cls : scope) {
    for (auto dmethod : cls->get_dmethods()) {
      if (methodFilter(dmethod)) {
        auto& code = dmethod->get_code();
        if (code) {
          for (auto& mie : *code->get_entries()) {
            if (mie.type != MFLOW_OPCODE) {
              continue;
            }
            opcodeWalker(dmethod, mie.insn);
          }
        }
      }
    }
    for (auto vmethod : cls->get_vmethods()) {
      if (methodFilter(vmethod)) {
        auto& code = vmethod->get_code();
        if (code) {
          for (auto& mie : *code->get_entries()) {
            if (mie.type != MFLOW_OPCODE) {
              continue;
            }
            opcodeWalker(vmethod, mie.insn);
          }
        }
      }
    }
  };
}

/**
 * Walk all annotations for all classes defined in 'scope' calling back the
 * walker function.
 */
template <class T, class AnnotationWalkerFn = void(DexAnnotation*)>
void call_annotation_walker(T* dex_thingy,
                            AnnotationWalkerFn annotationsWalker) {
  const auto& anno_set = dex_thingy->get_anno_set();
  if (!anno_set) return;
  auto& anno_list = anno_set->get_annotations();
  for (auto& anno : anno_list) {
    annotationsWalker(anno);
  }
}

template <class T, class AnnotationWalkerFn = void(DexAnnotation*)>
void walk_annotations(const T& scope, AnnotationWalkerFn annotation_walker) {
  for (auto& cls : scope) {
    call_annotation_walker(cls, annotation_walker);
  }
  walk_fields(scope,
              [&](DexField* field) {
                call_annotation_walker(field, annotation_walker);
              });
  walk_methods(scope,
               [&](DexMethod* method) {
                 call_annotation_walker(method, annotation_walker);
                 const auto& param_anno = method->get_param_anno();
                 if (!param_anno) return;
                 for (const auto& it : *param_anno) {
                   auto& anno_list = it.second->get_annotations();
                   for (auto& anno : anno_list) {
                     annotation_walker(anno);
                   }
                 }
               });
}

/**
 * Visit sequences of opcodes that satisfy the give matcher.
 *
 * Example
 * -------
 *
 * The following code (taken from ReachableClasses) visits all opcode sequences
 * that match the the form "const-string, invoke-static" where invoke-static is specifically
 * invoking Class.forName that takes one argument.
 *
 * In the walker callback, you can see that the opcodes are further inspected to ensure that
 * the register that const-string loads into is actually the register that is referenced by
 * invoke-static. (Without captures, this can't be expressed in the matcher language alone)
 *
 * The opcodes that match are passed in as a pointer to an array of DexInstruction pointers.
 * The size of the array is passed in as 'n'.
 *
 * Example Code
 * ------------
 *
 *  auto match = std::make_tuple(
 *    m::const_string(),
 *    m::invoke_static(
 *      m::opcode_method(
 *        m::named<DexMethod>("forName") &&
 *        m::on_class<DexMethod>("Ljava/lang/Class;"))
 *      && m::has_n_args(1))
 *  );
 *
 *  match_opcodes(scope, match, [&](const DexMethod* meth, size_t n, DexInstruction** insns){
 *    DexOpcodeString* const_string = (DexOpcodeString*)insns[0];
 *    DexOpcodeMethod* invoke_static = (DexOpcodeMethod*)insns[1];
 *    // Make sure that the registers agree
 *    if (const_string->dest() == invoke_static->src(0)) {
 *      TRACE(PGR, 1, "Class.forName: %s\n", const_string->get_string()->c_str());
 *    }
 *  });
 *
 */
template<
    typename P,
    size_t N = std::tuple_size<P>::value,
    typename V = void(const DexMethod*, size_t n, DexInstruction**)>
void walk_matching_opcodes(
  const Scope& scope, const P& p, const V& v) {
  walk_methods(
    scope,
    [&](const DexMethod* m) {
      auto& code = m->get_code();
      if (code) {
        std::vector<DexInstruction*> insns;
        for (auto& mie : InstructionIterable(code->get_entries())) {
          insns.emplace_back(mie.insn);
        }
        // No way to match if we have less insns than N
        if (insns.size() < N) {
          return;
        }
        // Try to match starting at i
        for (size_t i = 0 ; i <= insns.size() - N ; ++i) {
          if (m::insns_matcher<P, std::integral_constant<size_t, 0> >::matches_at(i, insns, p)) {
            DexInstruction* insns_array[N];
            for ( size_t c = 0 ; c < N ; ++c ) {
              insns_array[c] = insns.at(i+c);
            }
            v(m, N, insns_array);
          }
        }
      }
    });
}

/**
 * walker that respects basic block boundaries.
 *
 * It will not match a pattern that crosses block boundaries
 */
template<
    typename P,
    size_t N = std::tuple_size<P>::value,
    typename V = void(const DexMethod*, MethodTransform*, Block*, size_t n, DexInstruction**)>
void walk_matching_opcodes_in_block(
  const Scope& scope, const P& p, const V& v) {
  walk_methods(
    scope,
    [&](const DexMethod* m) {
      auto& code = m->get_code();
      if (code) {
        MethodTransform* mt = code->get_entries();
        mt->build_cfg();
        for (Block* block : mt->cfg().blocks()) {
          std::vector<DexInstruction*> insns;
          for (auto mie = block->begin() ; mie != block->end() ; mie++) {
            if (mie->type == MFLOW_OPCODE) {
              insns.emplace_back(mie->insn);
            }
          }
          // No way to match if we have less insns than N
          if (insns.size() >= N) {
            // Try to match starting at i
            for (size_t i = 0 ; i <= insns.size() - N ; ++i) {
              if (m::insns_matcher<P, std::integral_constant<size_t, 0> >::matches_at(i, insns, p)) {
                DexInstruction* insns_array[N];
                for ( size_t c = 0 ; c < N ; ++c ) {
                  insns_array[c] = insns.at(i+c);
                }
                v(m, mt, block, N, insns_array);
              }
            }
          }
        }
      }
    });
}
