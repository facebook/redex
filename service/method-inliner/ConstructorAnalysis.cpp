/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <utility>
#include <vector>

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>
#include <sparta/ReducedProductAbstractDomain.h>

#include "BaseIRAnalyzer.h"
#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "MethodUtil.h"
#include "ReachingDefinitions.h"
#include "Resolver.h"
#include "Show.h"
#include "Walkers.h"

using namespace sparta;

namespace {

using namespace ir_analyzer;

using BoolDomain = sparta::ConstantAbstractDomain<bool>;

/**
 * For each register, whether it represents the `this` parameter.
 **/
using ParamDomainEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, BoolDomain>;

// We track...
// - for each register, whether it represents the `this` parameter
// - whether the `this` object has been initialized by a constructor call
// - whether we came across something problematic that makes some constructor
//   call uninlinable
class ConstructorAnalysisEnvironment final
    : public sparta::ReducedProductAbstractDomain<
          ConstructorAnalysisEnvironment,
          ParamDomainEnvironment,
          BoolDomain,
          BoolDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;
  ConstructorAnalysisEnvironment()
      : ReducedProductAbstractDomain(
            std::make_tuple(ParamDomainEnvironment::top(),
                            BoolDomain(false),
                            BoolDomain(false))) {}

  static void reduce_product(
      std::tuple<ParamDomainEnvironment, BoolDomain, BoolDomain>&) {}

  const ParamDomainEnvironment& get_params() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const BoolDomain& get_initialized() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  const BoolDomain& get_uninlinable() const {
    return ReducedProductAbstractDomain::get<2>();
  }

  ConstructorAnalysisEnvironment& mutate_params(
      std::function<void(ParamDomainEnvironment*)> f) {
    apply<0>(std::move(f));
    return *this;
  }

  ConstructorAnalysisEnvironment& set_initialized(const BoolDomain& value) {
    apply<1>([&](BoolDomain* domain) { *domain = value; });
    return *this;
  }

  ConstructorAnalysisEnvironment& set_uninlinable(const BoolDomain& value) {
    apply<2>([&](BoolDomain* domain) { *domain = value; });
    return *this;
  }
};

const IRInstruction* get_first_load_param(const cfg::ControlFlowGraph& cfg) {
  const auto param_insns = InstructionIterable(cfg.get_param_instructions());
  auto& mie = *param_insns.begin();
  const auto insn = mie.insn;
  always_assert(insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT);
  return insn;
}

class Analyzer final : public BaseIRAnalyzer<ConstructorAnalysisEnvironment> {
 public:
  Analyzer(const cfg::ControlFlowGraph& cfg,
           const DexType* declaring_type,
           bool relaxed)
      : BaseIRAnalyzer(cfg),
        m_declaring_type(declaring_type),
        m_first_load_param(get_first_load_param(cfg)),
        m_relaxed(relaxed) {
    // We need to check superclass chain because dex spec allows calling
    // constructor on superclass of superclass
    // https://cs.android.com/android/platform/superproject/main/+/main:art/runtime/verifier/method_verifier.cc;l=2940
    auto super_cls_type = type_class(declaring_type)->get_super_class();
    while (super_cls_type && m_super_types.count(super_cls_type) == 0) {
      m_super_types.emplace(super_cls_type);
      auto super_cls = type_class(super_cls_type);
      if (!super_cls) {
        break;
      }
      super_cls_type = super_cls->get_super_class();
    }
    MonotonicFixpointIterator::run({});
  }

  void analyze_instruction(
      const IRInstruction* insn,
      ConstructorAnalysisEnvironment* current_state) const override {
    // Once `this` has been initialized, or we came across something problematic
    // that prevents inlining (before the instance got initialized), then
    // there's no point of continuing our analysis, and we can stop mutating
    // the tracked state. (And we don't have worry about what it means to evolve
    // a "terminal" state.)
    auto initialized = current_state->get_initialized();
    auto uninlinable = current_state->get_uninlinable();
    if (!initialized.get_constant() || *initialized.get_constant() ||
        !uninlinable.get_constant() || *uninlinable.get_constant()) {
      return;
    }

    const auto set_current_state_at = [&](reg_t reg, bool wide,
                                          BoolDomain value) {
      current_state->mutate_params([&](ParamDomainEnvironment* env) {
        env->set(reg, value);
        if (wide) {
          env->set(reg + 1, BoolDomain::top());
        }
      });
    };

    auto opcode = insn->opcode();
    if (opcode::is_a_move(opcode)) {
      const auto value = current_state->get_params().get(insn->src(0));
      set_current_state_at(insn->dest(), insn->dest_is_wide(), value);
      return;
    } else if (opcode::is_an_iput(opcode)) {
      auto field_ref = insn->get_field();
      DexField* field = resolve_field(field_ref, FieldSearch::Instance);
      if (field == nullptr || field->get_class() == m_declaring_type) {
        auto object_value = current_state->get_params().get(insn->src(1));
        if (!object_value.get_constant() || *object_value.get_constant()) {
          // This particular analysis is used to determine if a constructor is
          // generally inlinable in all possible caller contexts.
          // This is not possible if there are instance field assignments before
          // another constructor is called. Such instance field assignments are
          // only legal in a constructor (declared in the type in which the
          // instance fields were declared), but wouldn't be legal when that
          // code is inlined into a non-constructor context.
          // See B8 here:
          // https://source.android.com/devices/tech/dalvik/constraints Thus, we
          // give up if there's a possible assignment to a field of the
          // declaring class before another constructor was called
          current_state->set_uninlinable(BoolDomain(true));
          return;
        }
      }
      // otherwise, fall through
    } else if (opcode == OPCODE_INVOKE_DIRECT) {
      auto method_ref = insn->get_method();
      if (method::is_init(method_ref)) {
        DexMethod* method = resolve_method(method_ref, MethodSearch::Direct);
        if (method == nullptr) {
          current_state->set_uninlinable(BoolDomain(true));
          return;
        }
        auto method_class = method->get_class();

        if (method_class == m_declaring_type ||
            m_super_types.count(method_class)) {
          auto first_param = current_state->get_params().get(insn->src(0));
          if (!first_param.get_constant() || *first_param.get_constant()) {
            // We've encountered a call to another constructor on a value
            // that might be `this`
            if ((!m_relaxed && m_super_types.count(method_class)) ||
                !first_param.get_constant()) {
              current_state->set_uninlinable(BoolDomain(true));
            } else {
              current_state->set_initialized(BoolDomain(true));
            }
            return;
          }
        }
      }
      // otherwise, fall through
    }

    if (insn->has_dest()) {
      bool is_first_parameter = insn == m_first_load_param;
      const auto value = BoolDomain(is_first_parameter);
      set_current_state_at(insn->dest(), insn->dest_is_wide(), value);
    }
  }

 private:
  const DexType* m_declaring_type;
  UnorderedSet<DexType*> m_super_types;
  const IRInstruction* m_first_load_param;
  bool m_relaxed;
};
} // namespace

////////////////////////////////////////////////////////////////////////////////

namespace constructor_analysis {

bool can_inline_init(const DexMethod* init_method,
                     const UnorderedSet<const DexField*>* finalizable_fields,
                     bool relaxed,
                     UnorderedSet<DexField*>* written_final_fields) {
  always_assert(method::is_init(init_method));
  auto code = init_method->get_code();
  if (!code) {
    return false;
  }
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  DexType* declaring_type = init_method->get_class();
  Analyzer analyzer(cfg, declaring_type, relaxed);
  for (const auto block : cfg.blocks()) {
    const auto& env = analyzer.get_exit_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    const auto& uninlinable = env.get_uninlinable();
    always_assert(!uninlinable.is_bottom());
    if (uninlinable.is_top() || *uninlinable.get_constant()) {
      return false;
    }
    if (block->branchingness() == opcode::Branchingness::BRANCH_RETURN) {
      const auto& initialized = env.get_initialized();
      always_assert(!initialized.is_bottom());
      if (initialized.is_top()) {
        // Shouldn't happen, but we play it safe.
        return false;
      }
      always_assert_log(
          *initialized.get_constant(),
          "%s returns at %p: %s without having called an appropriate "
          "constructor from the same or its immediate super "
          "class. This indicates malformed DEX code.\n%s",
          SHOW(init_method), block->get_last_insn()->insn,
          SHOW(block->get_last_insn()->insn), SHOW(cfg));
    }
  }
  bool res = true;
  for (const auto& mie : InstructionIterable(cfg)) {
    auto insn = mie.insn;
    if (opcode::is_an_iput(insn->opcode())) {
      auto field_ref = insn->get_field();
      DexField* field = resolve_field(field_ref, FieldSearch::Instance);
      if (field == nullptr ||
          (field->get_class() == declaring_type &&
           (is_final(field) ||
            (finalizable_fields && finalizable_fields->count(field))))) {
        if (written_final_fields) {
          written_final_fields->emplace(field);
        }
        res = false;
      }
    }
  }
  return res;
}

bool can_inline_inits_in_same_class(DexMethod* caller_method,
                                    const DexMethod* callee_method,
                                    IRInstruction* callsite_insn) {
  always_assert(method::is_init(caller_method));
  always_assert(caller_method->get_class() == callee_method->get_class());
  auto code = caller_method->get_code();
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  reaching_defs::MoveAwareFixpointIterator reaching_definitions(cfg);
  reaching_definitions.run(reaching_defs::Environment());

  auto first_load_param = cfg.get_param_instructions().begin()->insn;
  always_assert(first_load_param->opcode() == IOPCODE_LOAD_PARAM_OBJECT);
  auto in_block = [&](cfg::Block* block) {
    auto env = reaching_definitions.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      bool matches;
      if (callsite_insn != nullptr) {
        matches = (insn == callsite_insn);
      } else {
        matches = insn->opcode() == OPCODE_INVOKE_DIRECT &&
                  insn->get_method() == callee_method;
      }
      if (matches) {
        auto defs = env.get(insn->src(0));
        if (defs.is_top() || defs.is_bottom()) {
          return false;
        } else {
          for (auto def : defs.elements()) {
            if (def != first_load_param) {
              return false;
            }
          }
        }
      }
      reaching_definitions.analyze_instruction(insn, &env);
    }
    return true;
  };
  if (callsite_insn != nullptr) {
    always_assert(callsite_insn->opcode() == OPCODE_INVOKE_DIRECT);
    always_assert(callsite_insn->get_method() == callee_method);
    auto it = cfg.find_insn(callsite_insn);
    always_assert(it != InstructionIterable(cfg).end());
    auto block = it.block();
    return in_block(block);
  } else {
    for (cfg::Block* block : cfg.blocks()) {
      if (!in_block(block)) {
        return false;
      }
    }
    return true;
  }
}

UnorderedSet<const DexType*> find_complex_init_inlined_types(
    const std::vector<DexClass*>& scope) {
  InsertOnlyConcurrentSet<const DexType*> items;
  // Calling this on an unknown type is apparently OK for verification.
  auto object_init = DexMethod::get_method("Ljava/lang/Object;.<init>:()V");
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code == nullptr) {
      return;
    }
    always_assert(code->editable_cfg_built());
    auto& cfg = code->cfg();
    Lazy<live_range::LazyLiveRanges> live_ranges(
        [&]() { return std::make_unique<live_range::LazyLiveRanges>(cfg); });
    for (auto* block : cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_NEW_INSTANCE) {
          auto new_instance_type = insn->get_type();
          auto search = live_ranges->def_use_chains->find(insn);
          if (search != live_ranges->def_use_chains->end()) {
            for (auto& use : UnorderedIterable(search->second)) {
              if (use.insn->has_method()) {
                auto use_method = use.insn->get_method();
                if (use.src_index == 0 && method::is_init(use_method) &&
                    use_method != object_init &&
                    use_method->get_class() != new_instance_type) {
                  items.insert(new_instance_type);
                }
              }
            }
          }
        }
      }
    }
  });
  UnorderedSet<const DexType*> result;
  insert_unordered_iterable(result, items);
  return result;
}
} // namespace constructor_analysis
