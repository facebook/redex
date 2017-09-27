/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>

#include "Debug.h"
#include "DexClass.h"
#include "IRInstruction.h"

/*
 * This is the implementation of a type checker for the IR that aims at
 * detecting errors such as fetching a reference from a register containing an
 * integer, or incorrectly accessing the pair of registers holding a wide value.
 * Our purpose is not to replicate the Android verifier, which performs
 * fine-grained compatibility checks on scalars of various bitwidths and tracks
 * the precise type of references. This type checker is intended to be used as a
 * sanity check at the end of an optimization pass and it has been designed for
 * high performance. For more information, please see the following link, which
 * points to the code of the Android verifier (there is no requirements
 * specification document):
 *
 *   androidxref.com/7.1.1_r6/xref/art/runtime/verifier/method_verifier.cc
 *
 * We first infer the type of all registers by performing a monotonic fixpoint
 * iteration sequence over the following type lattice:
 *
 *
 *                                   TOP
 *                                    |
 *         +-----------+--------------+---------------+
 *         |           |              |               |
 *         |         SCALAR        SCALAR1         SCALAR2
 *         |         /   \          /    \          /    \
 *         |        /     \        /      \        /      \
 *     REFERENCE  INT    FLOAT  LONG1  DOUBLE1  LONG2  DOUBLE2
 *         |        \     /        \      /        \      /
 *         |         \   /          \    /          \    /
 *         |         CONST          CONST1          CONST2
 *         |           |              |               |
 *         |           |              |               |
 *         +-----+-----+              |               |
 *               |                    |               |
 *             ZERO                   |               |
 *               |                    |               |
 *               +--------------------+---------------+
 *                                    |
 *                                  BOTTOM
 *
 *
 * The const-* operations of the Dalvik bytecode are not typed. Until the
 * context provides additional information, a constant (resp. wide constant) can
 * be either a long or a float (resp. a long or a double), hence the need for
 * the CONST types. The value of the constant doesn't matter unless it's a
 * 32-bit constant equal to 0. Zero needs to be treated specially because it's
 * also used to represent the null reference. Since a type is attached to a
 * 32-bit register, we need two mirror sets of distinct types for a pair of
 * registers holding a wide value.
 *
 * The SCALAR types are required to handle array access operations properly.
 * Although most aget-* operations are typed, this is not the case for aget (the
 * same holds for aput-* operations). This operation can retrieve either an
 * integer or a floating-point number, depending on the type of the array
 * (aget-wide has the same issue). The Android verifier tracks the precise type
 * of references and is able to resolve the type of the element statically. We
 * don't want to do that here for efficiency purposes, hence the need for the
 * SCALAR types, which encode the uncertainty about the actual type of the array
 * element. This means that the checker might not be able to detect type errors
 * in these situations. However, we can refine these types as soon as the
 * context provides enough information.
 *
 * Example:
 *   aget v0, v1, v2     --> v0 has SCALAR type
 *   add-int v3, v0, v4  --> we don't know whether v0 holds an integer value
 *   mul-int v5, v0, v4  --> if we reach this instruction, it means that v0 did
 *   ...                     hold an integer value (otherwise the code would
 *                           have been rejected by the Android verifier). This
 *                           operation is therefore well typed.
 *
 *   We don't know if the the first use of a register with SCALAR type in a
 *   typed context is correct. However, after that first use, the register's
 *   type is fully determined. If the first use of the register was erroneous it
 *   would be rejected by the Android verifier and hence, all subsequent uses
 *   would never be executed.
 *
 * As is standard in Abstract Interpretation, the BOTTOM type corresponds to
 * unreachable code. Note that any code that follows a type error is interpreted
 * as unreachable (because it can never be executed). The TOP type corresponds
 * to an undefined behavior, because the register either hasn't been initialized
 * or holds different types along the branches of a merge node in the
 * control-flow graph.
 */

enum IRType {
  BOTTOM,
  ZERO,
  CONST,
  CONST1,
  CONST2,
  REFERENCE,
  INT,
  FLOAT,
  LONG1,
  LONG2,
  DOUBLE1,
  DOUBLE2,
  SCALAR,
  SCALAR1,
  SCALAR2,
  TOP
};

std::ostream& operator<<(std::ostream& output, const IRType& type);

namespace irtc_impl {

// Forward declaration
class TypeInference;

} // namespace irtc_impl

/*
 * This class takes a method, infers the type of all registers and checks that
 * all operations are well typed. The inferred types are available via the
 * `get_type` method and can be used by optimization/analysis passes that
 * require type information. Note that the type checker stops at the first error
 * encountered.
 *
 * IMPORTANT: the type checker assumes that invoke-* instructions are in
 * denormalized form, i.e., wide arguments are explicitly represented by a pair
 * of consecutive registers. The type checker doesn't modify the IR and hence,
 * can be used anywhere in Redex.
 */
class IRTypeChecker final {
 public:
  // If we don't declare a destructor for this class, a default destructor will
  // be generated by the compiler, which requires a complete definition of
  // TypeInference, thus causing a compilation error. Note that the destructor's
  // definition must be located after the definition of TypeInference.
  ~IRTypeChecker();

  explicit IRTypeChecker(DexMethod* dex_method);

  IRTypeChecker(const IRTypeChecker&) = delete;

  IRTypeChecker& operator=(const IRTypeChecker&) = delete;

  /*
   * The Android verifier doesn't consider constants to be polymorphic. For
   * example, the following piece of code doesn't pass verification:
   *
   *   const v0, 0
   *   check-cast v0, Ljava/lang/String;
   *   add-int-2addr v1, v0
   *
   * After the check-cast instruction, v0 is assumed to contain a reference and
   * cannot be used in an arithmetic operation. By default, the type checker
   * complies with the Android verifier. Calling this method mutes the check.
   * This is useful when the verify_none mode is enabled, for example.
   */
  void enable_polymorphic_constants() {
    if (!m_complete) {
      // We can only set this parameter before running the type checker.
      m_enable_polymorphic_constants = true;
    }
  }

  /*
   * TOP represents an undefined value and hence, should never occur as the type
   * of a register. However, the Android verifier allows one exception, when an
   * undefined value is used as the operand of a move-* instruction (TOP is
   * named 'conflict' in the dataflow framework used by the Android verifier):
   *
   * http://androidxref.com/7.1.1_r6/xref/art/runtime/verifier/register_line-inl.h#101
   *
   * By default, the type checker complies with the Android verifier. Calling
   * this method enables a stricter check of move-* instructions: using a
   * register holding an undefined value in a move-* will result into a type
   * error.
   */
  void verify_moves() {
    if (!m_complete) {
      // We can only set this parameter before running the type checker.
      m_verify_moves = true;
    }
  }

  void run();

  bool good() const {
    check_completion();
    return m_good;
  }

  bool fail() const {
    check_completion();
    return !m_good;
  }

  /*
   * Returns a legible description of the type error, or "OK" otherwise. Note
   * that type checking aborts at the first error encountered.
   */
  std::string what() const {
    check_completion();
    return m_what;
  }

  /*
   * Returns the type of a register at the given instruction. Note that the type
   * returned is that of the register _before_ the instruction is executed. For
   * example, if we query the type of v0 in the following instruction:
   *
   *   aget-object v0, v1, v0
   *
   * we will get INT and not REFERENCE, which would be the type of v0 _after_
   * the instruction has been executed.
   */
  IRType get_type(IRInstruction* insn, uint16_t reg) const;

 private:
  void check_completion() const {
    always_assert_log(m_complete,
                      "The type checker did not run on method %s.\n",
                      m_dex_method->get_deobfuscated_name().c_str());
  }

  DexMethod* m_dex_method;
  bool m_complete;
  bool m_enable_polymorphic_constants;
  bool m_verify_moves;
  bool m_good;
  std::string m_what;
  std::unique_ptr<irtc_impl::TypeInference> m_type_inference;

  friend std::ostream& operator<<(std::ostream&, const IRTypeChecker&);
};

std::ostream& operator<<(std::ostream& output, const IRTypeChecker& checker);
