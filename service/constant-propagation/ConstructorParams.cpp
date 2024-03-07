/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstructorParams.h"

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>
#include <sparta/ReducedProductAbstractDomain.h>

#include "BaseIRAnalyzer.h"
#include "ConstantPropagationAnalysis.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Walkers.h"

// Analyze a constructor to learn how the instance fields are initialized.
// It understands patterns like
// load-param v1
// ...
// load-param v0
// ...
// # Not in a branch.
// iput v0 v1 field
//  -or-
// invoke-direct v1 v0 SameClass.<init>:(I)V
//
// For determinism, currently not understand the fields defined in internal
// super classes.

namespace {
using ParamIdxDomain = sparta::ConstantAbstractDomain<size_t>;
using RegisterEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, ParamIdxDomain>;
using FieldEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<DexField*, ParamIdxDomain>;
// For some invisible instance fields like Enum.name and Enum.ordinal, we use
// public getter methods to represent them.
using InvisibleFieldEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<DexMethod*, ParamIdxDomain>;

class Environment final
    : public sparta::ReducedProductAbstractDomain<Environment,
                                                  RegisterEnvironment,
                                                  FieldEnvironment,
                                                  InvisibleFieldEnvironment> {
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

 public:
  static void reduce_product(std::tuple<RegisterEnvironment,
                                        FieldEnvironment,
                                        InvisibleFieldEnvironment>&) {}

  ParamIdxDomain get(reg_t reg) const {
    return ReducedProductAbstractDomain::get<0>().get(reg);
  }

  void set(reg_t reg, ParamIdxDomain value) {
    apply<0>([&](RegisterEnvironment* env) { env->set(reg, value); });
  }

  void set(DexField* field, ParamIdxDomain value) {
    apply<1>([&](FieldEnvironment* env) { env->set(field, value); });
  }

  const FieldEnvironment& get_field_environment() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  void set(DexMethod* method, ParamIdxDomain value) {
    apply<2>([&](InvisibleFieldEnvironment* env) { env->set(method, value); });
  }

  const InvisibleFieldEnvironment& get_invisible_field_environment() const {
    return ReducedProductAbstractDomain::get<2>();
  }
};

class InitFixpointIterator final
    : public ir_analyzer::BaseIRAnalyzer<Environment> {
  const DexType* m_current_cls;
  const constant_propagation::ImmutableAttributeAnalyzerState& m_state;

 public:
  InitFixpointIterator(
      const cfg::ControlFlowGraph& cfg,
      const DexType* cls,
      const constant_propagation::ImmutableAttributeAnalyzerState& state)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
        m_current_cls(cls),
        m_state(state) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* env) const override {
    auto opcode = insn->opcode();
    if (opcode::is_a_load_param(opcode)) {
      return;
    } else if (opcode::is_a_move(opcode)) {
      env->set(insn->dest(), env->get(insn->src(0)));
      return;
    } else if (opcode::is_an_iput(opcode)) {
      // Is writing to `this` pointer.
      const auto& obj_domain = env->get(insn->src(1));
      if (obj_domain.is_value() && *obj_domain.get_constant() == 0) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
        if (!field) {
          field = static_cast<DexField*>(insn->get_field());
        } else if (field->get_class() != m_current_cls &&
                   !field->is_external()) {
          return;
        }
        env->set(field, env->get(insn->src(0)));
      }
      return;
    } else if (opcode::is_invoke_direct(opcode) &&
               method::is_init(insn->get_method())) {
      // Another construction invocation on `this` pointer.
      const auto& obj_domain = env->get(insn->src(0));
      if (obj_domain.is_value() && *obj_domain.get_constant() == 0) {
        auto method = resolve_method(insn->get_method(), MethodSearch::Direct);
        if (!method) {
          method = static_cast<DexMethod*>(insn->get_method());
        } else if (method->get_class() != m_current_cls &&
                   !method->is_external()) {
          env->set(RESULT_REGISTER, ParamIdxDomain::top());
          return;
        }
        std::unique_lock<std::mutex> lock(
            m_state.method_initializers.get_lock(method));
        auto it = m_state.method_initializers.find(method);
        if (it != m_state.method_initializers.end()) {
          for (auto& initializer : it->second) {
            if (initializer->attr.is_method()) {
              env->set(initializer->attr.val.method,
                       env->get(insn->src(initializer->insn_src_id_of_attr)));
            } else { // is_field
              env->set(initializer->attr.val.field,
                       env->get(insn->src(initializer->insn_src_id_of_attr)));
            }
          }
          auto& first_initializer = *it->second.begin();
          if (first_initializer->obj_is_dest()) {
            env->set(RESULT_REGISTER, obj_domain);
            return;
          }
        }
      }
    }
    if (insn->has_dest()) {
      env->set(insn->dest(), ParamIdxDomain::top());
    } else if (insn->has_move_result_any()) {
      env->set(RESULT_REGISTER, ParamIdxDomain::top());
    }
  }
};

/**
 * Return an ordered vector, ordered by address of Attr.
 */
std::vector<std::pair<ImmutableAttr::Attr, size_t>> analyze_initializer(
    const DexMethod* method,
    const constant_propagation::ImmutableAttributeAnalyzerState& state,
    const std::unordered_set<DexField*>& final_fields) {
  std::vector<std::pair<ImmutableAttr::Attr, size_t>> usage;
  Environment init_env;
  size_t param_id = 0;
  cfg::ScopedCFG cfg(const_cast<IRCode*>(method->get_code()));
  for (const auto& mie : cfg->get_param_instructions()) {
    init_env.set(mie.insn->dest(), ParamIdxDomain(param_id++));
  }
  InitFixpointIterator fp_iter(*cfg, method->get_class(), state);
  fp_iter.run(init_env);
  auto return_env = Environment::bottom();
  for (cfg::Block* block : cfg->blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      fp_iter.analyze_instruction(insn, &env);
      if (opcode::is_a_return(insn->opcode())) {
        return_env.join_with(env);
      }
    }
  }
  const auto& field_env = return_env.get_field_environment();
  if (field_env.is_value()) {
    for (const auto& pair : field_env.bindings()) {
      auto field = pair.first;
      auto value = pair.second.get_constant();
      if (!final_fields.count(field)) {
        continue;
      }
      if (value) {
        ImmutableAttr::Attr attr(field);
        usage.emplace_back(std::make_pair(attr, *value));
      }
    }
  }
  const auto& invisible_field_env =
      return_env.get_invisible_field_environment();
  if (invisible_field_env.is_value()) {
    for (const auto& pair : invisible_field_env.bindings()) {
      auto method_attr = pair.first;
      auto value = pair.second.get_constant();
      ImmutableAttr::Attr attr(method_attr);
      if (value) {
        usage.emplace_back(std::make_pair(attr, *value));
      }
    }
  }
  std::sort(usage.begin(), usage.end());
  return usage;
}
} // namespace

namespace constant_propagation {
namespace immutable_state {
void analyze_constructors(const Scope& scope,
                          ImmutableAttributeAnalyzerState* state) {
  // java.lang.Enum is the super class of enums, `Enum.<init>(String, int)`
  // invocation is to initialize `ordinal` and `name` fields. Given this input,
  // the analysis part learns about the invisible fields initialization when
  // analyzing enum constructors.
  state
      ->add_initializer(method::java_lang_Enum_ctor(),
                        method::java_lang_Enum_name())
      .set_src_id_of_obj(0)
      .set_src_id_of_attr(1);
  state
      ->add_initializer(method::java_lang_Enum_ctor(),
                        method::java_lang_Enum_ordinal())
      .set_src_id_of_obj(0)
      .set_src_id_of_attr(2);
  auto java_lang_String = type::java_lang_String();
  walk::parallel::classes(scope, [state, java_lang_String](DexClass* cls) {
    std::unordered_set<DexField*> fields;
    for (auto ifield : cls->get_ifields()) {
      if (is_final(ifield) && !root(ifield) &&
          (type::is_primitive(ifield->get_type()) ||
           ifield->get_type() == java_lang_String)) {
        fields.insert(ifield);
      }
    }
    if (fields.empty() && !is_enum(cls)) {
      return;
    }
    auto ctors = cls->get_ctors();
    for (auto ctor : ctors) {
      for (const auto& pair : analyze_initializer(ctor, *state, fields)) {
        auto& attr = pair.first;
        auto attr_param_idx = pair.second;
        state->add_initializer(ctor, attr)
            .set_src_id_of_obj(0)
            .set_src_id_of_attr(attr_param_idx);
      }
    }
  });
}
} // namespace immutable_state
} // namespace constant_propagation
