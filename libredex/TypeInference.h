/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional/optional_io.hpp>
#include <ostream>

#include "BaseIRAnalyzer.h"
#include "DexTypeEnvironment.h"
#include "FiniteAbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"

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

namespace type_inference {

using std::placeholders::_1;

using TypeLattice = sparta::BitVectorLattice<IRType, 16, std::hash<int>>;

extern TypeLattice type_lattice;

using TypeDomain = sparta::FiniteAbstractDomain<IRType,
                                                TypeLattice,
                                                TypeLattice::Encoding,
                                                &type_lattice>;

using namespace ir_analyzer;

using BasicTypeEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, TypeDomain>;

/*
 * Note that we only track the register DexTypeDomain mapping here. We always
 * take the declared DexType when reading a field. We do not track more precise
 * DexType for fields for individual intraprocedural analysis.
 * The reason is that the analysis can be incomplete. A field can potentially
 * be written by another thread concurrently. That write is visible to the
 * reads of the method we are analyzing currently. We could lose type
 * information if we don't consider write from other methods. Therefore, we stay
 * with the declared field type for local type inference.
 */
class TypeEnvironment final
    : public sparta::ReducedProductAbstractDomain<TypeEnvironment,
                                                  BasicTypeEnvironment,
                                                  RegTypeEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      std::tuple<BasicTypeEnvironment, RegTypeEnvironment>& /* product */) {}

  TypeDomain get_type(reg_t reg) const { return get<0>().get(reg); }

  void set_type(reg_t reg, const TypeDomain type) {
    apply<0>([=](auto env) { env->set(reg, type); }, true);
  }

  void update_type(
      reg_t reg,
      const std::function<TypeDomain(const TypeDomain&)>& operation) {
    apply<0>([=](auto env) { env->update(reg, operation); }, true);
  }

  boost::optional<const DexType*> get_dex_type(reg_t reg) const {
    return get<1>().get(reg).get_dex_type();
  }

  DexTypeDomain get_type_domain(reg_t reg) const { return get<1>().get(reg); }

  void set_dex_type(reg_t reg, const DexTypeDomain& dex_type) {
    apply<1>([=](auto env) { env->set(reg, dex_type); }, true);
  }

  void reset_dex_type(reg_t reg) {
    apply<1>([=](auto env) { env->set(reg, DexTypeDomain::top()); }, true);
  }
};

class TypeInference final
    : public ir_analyzer::BaseIRAnalyzer<TypeEnvironment> {
 public:
  explicit TypeInference(const cfg::ControlFlowGraph& cfg,
                         bool skip_check_cast_to_intf = false)
      : ir_analyzer::BaseIRAnalyzer<TypeEnvironment>(cfg),
        m_cfg(cfg),
        m_skip_check_cast_to_intf(skip_check_cast_to_intf) {}

  void run(const DexMethod* dex_method);

  void run(bool is_static, DexType* declaring_type, DexTypeList* args);

  void analyze_node(const cfg::GraphInterface::NodeId& node,
                    TypeEnvironment* current_state) const override {
    for (auto& mie : InstructionIterable(node)) {
      analyze_instruction(mie.insn, current_state, node);
    }
  }

  void analyze_instruction(const IRInstruction* insn,
                           TypeEnvironment* current_state) const override {

    analyze_instruction(insn, current_state, nullptr);
  }

  void analyze_instruction(const IRInstruction* insn,
                           TypeEnvironment* current_state,
                           const cfg::Block* current_block) const;

  void print(std::ostream& output) const;

  void traceState(TypeEnvironment* state) const;

  std::unordered_map<const IRInstruction*, TypeEnvironment>&
  get_type_environments() {
    return m_type_envs;
  }

 private:
  void populate_type_environments();

  const cfg::ControlFlowGraph& m_cfg;
  std::unordered_map<const IRInstruction*, TypeEnvironment> m_type_envs;
  const bool m_skip_check_cast_to_intf;

  TypeDomain refine_type(const TypeDomain& type,
                         IRType expected,
                         IRType const_type,
                         IRType scalar_type) const;
  void refine_type(TypeEnvironment* state, reg_t reg, IRType expected) const;
  void refine_wide_type(TypeEnvironment* state,
                        reg_t reg,
                        IRType expected1,
                        IRType expected2) const;
  void refine_reference(TypeEnvironment* state, reg_t reg) const;
  void refine_scalar(TypeEnvironment* state, reg_t reg) const;
  void refine_integer(TypeEnvironment* state, reg_t reg) const;
  void refine_float(TypeEnvironment* state, reg_t reg) const;
  void refine_wide_scalar(TypeEnvironment* state, reg_t reg) const;
  void refine_long(TypeEnvironment* state, reg_t reg) const;
  void refine_double(TypeEnvironment* state, reg_t reg) const;
};

} // namespace type_inference
