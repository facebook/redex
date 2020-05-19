/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EvaluateTypeChecks.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <boost/optional.hpp>

#include "CFGMutation.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "ConstantPropagationWholeProgramState.h"
#include "CopyPropagation.h"
#include "CppUtil.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "LocalDce.h"
#include "PassManager.h"
#include "ReachingDefinitions.h"
#include "ScopedCFG.h"
#include "Trace.h"
#include "TypeInference.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace check_casts {

namespace {

using namespace cfg;
using namespace type_inference;

void print_type_chain(std::ostream& os, const DexType* type, size_t indent) {
  if (type == nullptr) {
    return;
  }
  for (size_t i = 0; i != indent; ++i) {
    os << ' ';
  }
  os << show(type);
  auto kls = type_class(type);
  if (kls) {
    if (kls->is_external()) {
      os << " (E)";
    }
    os << '\n';
    print_type_chain(os, kls->get_super_class(), indent + 1);
  } else {
    os << '\n';
  }
}

boost::optional<int32_t> evaluate_impl(const DexType* src_type,
                                       const DexType* test_type) {
  if (test_type == src_type) {
    // Trivial.
    return 1;
  }

  auto test_cls = type_class(test_type);
  if (test_cls == nullptr) {
    return boost::none;
  }

  auto src_cls = type_class(src_type);
  if (src_cls == nullptr) {
    return boost::none;
  }

  // OK, let's simplify for now. While some SDK classes should be set in
  // stone, let's only work on internals.
  if (test_cls->is_external() || src_cls->is_external()) {
    return boost::none;
  }

  // Class vs class, for simplicity.
  if (!is_interface(test_cls) && !is_interface(src_cls)) {
    if (type::check_cast(src_cls->get_type(), test_cls->get_type())) {
      // If check-cast succeeds, the result will be `true`.
      return 1;
    } else if (!type::check_cast(test_cls->get_type(), src_cls->get_type())) {
      // The check can never succeed, as the test class is not a subtype.
      return 0;
    }
    return boost::none;
  }

  return boost::none;
}

struct RemoveResult {
  size_t methods_w_instanceof{0};
  size_t overrides{0};
  size_t class_always_succeed_or_null{0};
  size_t class_always_succeed_or_null_repl{0};
  size_t class_always_fail{0};
  size_t def_use_loop{0};
  size_t multi_use{0};
  size_t non_move{0};
  size_t non_branch{0};
  size_t non_supported_branch{0};
  ssize_t insn_delta{0};

  RemoveResult& operator+=(const RemoveResult& rhs) {
    methods_w_instanceof += rhs.methods_w_instanceof;
    overrides += rhs.overrides;
    class_always_succeed_or_null += rhs.class_always_succeed_or_null;
    class_always_succeed_or_null_repl += rhs.class_always_succeed_or_null_repl;
    class_always_fail += rhs.class_always_fail;
    def_use_loop += rhs.def_use_loop;
    multi_use += rhs.multi_use;
    non_move += rhs.non_move;
    non_branch += rhs.non_branch;
    non_supported_branch += rhs.non_supported_branch;
    insn_delta += rhs.insn_delta;
    return *this;
  }
};

using DefUses =
    std::unordered_map<IRInstruction*, std::unordered_set<IRInstruction*>>;

DefUses compute_def_uses(ControlFlowGraph& cfg) {
  using namespace reaching_defs;
  FixpointIterator fixpoint_iter{cfg};
  fixpoint_iter.run(Environment());
  DefUses res;
  for (auto block : cfg.blocks()) {
    auto defs_in = fixpoint_iter.get_entry_state_at(block);
    for (const auto& mie : ir_list::InstructionIterable(block)) {
      auto insn = mie.insn;
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        auto src_reg = insn->src(i);
        auto defs = defs_in.get(src_reg);
        always_assert_log(!defs.is_top() && defs.size() > 0,
                          "Found use without def when processing [0x%lx]%s",
                          &mie, SHOW(insn));
        for (auto def : defs.elements()) {
          res[def].insert(insn);
        }
      }
      fixpoint_iter.analyze_instruction(insn, &defs_in);
    }
  }
  return res;
}

// Follow def-use chains. Find a terminal use. Only allow moves along
// the chain.
IRInstruction* find_single_terminal_use(IRInstruction* start,
                                        const DefUses& def_uses,
                                        RemoveResult& res) {
  IRInstruction* insn = start;
  std::unordered_set<IRInstruction*> seen;
  for (;;) {
    if (seen.count(insn) != 0) {
      ++res.def_use_loop;
      return nullptr;
      break;
    }

    seen.insert(insn);
    auto it = def_uses.find(insn);
    if (it == def_uses.end() || it->second.empty()) {
      // Terminal use.
      return insn;
    }
    if (it->second.size() > 1) {
      // Register is used by multiple instructions, not a simple chain.
      ++res.multi_use;
      return nullptr;
    }
    auto opcode = insn->opcode();
    if (!is_move(opcode) && !opcode::is_move_result_pseudo(opcode)) {
      // Not a move. Don't know what happens to the value.
      ++res.non_move;
      return nullptr;
    }

    insn = *it->second.begin();
  }
  not_reached();
}

// If we know that an instance-of will always be true (if the value is not
// null), then it may be beneficial to rewrite the code. The instance-of is
// basically a null-check, which is computationally simpler.
//
// However, instance-of produces a boolean value, while a null-check is a
// conditional branch. If the boolean value is actually used for more than a
// branch, it is a tighter encoding (though more expensive at runtime).
//
// The simple approach here figures out if the output of the instance-of only
// flows into an if-eqz or if-nez directly, and in that case uses the value
// directly. The instance-of can then be eliminated, saving space (and
// increasing speed). Follow-up analyzes and optimizations might take advantage
// of the simpler code, e.g., when it can be shown that the receiver is (or is
// not) null.
void analyze_true_instance_ofs(
    ControlFlowGraph& cfg,
    CFGMutation& mutation,
    RemoveResult& res,
    const std::unordered_set<const MethodItemEntry*>& true_modulo_null_set) {
  if (true_modulo_null_set.empty()) {
    return;
  }
  auto def_uses = compute_def_uses(cfg);
  for (const auto* mie : true_modulo_null_set) {
    auto def_it = cfg.find_insn(mie->insn);
    auto move_it = cfg.move_result_of(def_it);
    if (move_it.is_end()) { // Should not happen.
      continue;
    }

    IRInstruction* insn =
        find_single_terminal_use(move_it->insn, def_uses, res);
    if (insn == nullptr) {
      continue;
    }

    auto opcode = insn->opcode();
    if (!is_conditional_branch(opcode)) {
      TRACE(EVALTC, 3, "Not a branch: %s", SHOW(insn));
      ++res.non_branch;
      continue;
    }

    if (opcode != OPCODE_IF_EQZ && opcode != OPCODE_IF_NEZ) {
      TRACE(EVALTC, 2, "Unexpected branch type: %s", SHOW(insn));
      ++res.non_supported_branch;
      continue;
    }

    // v1 := instance-of v0 == v0 != null.
    //  -> if-eqz v1 ~= if-eqz v0
    //  -> if-nez v1 ~= if-nez v0
    //
    // Need a temp to have access to unmodified v0.
    reg_t src_tmp = cfg.allocate_temp();
    {
      auto copy_reg_insn = new IRInstruction(OPCODE_MOVE_OBJECT);
      copy_reg_insn->set_src(0, mie->insn->src(0));
      copy_reg_insn->set_dest(src_tmp);
      mutation.insert_before(def_it, {copy_reg_insn});
    }
    // Rewrite the conditional's input.
    insn->set_src(0, src_tmp);
    ++res.class_always_succeed_or_null_repl;
    ++res.overrides;
  }
}

RemoveResult analyze_and_evaluate(DexMethod* method) {
  ScopedCFG cfg(method->get_code());
  CFGMutation mutation(*cfg);

  RemoveResult res;
  std::unordered_set<const MethodItemEntry*> true_modulo_null_set;

  // Figure out types. Find guaranteed-false checks.
  {
    TypeInference type_inf(*cfg);
    type_inf.run(method);

    auto& type_envs = type_inf.get_type_environments();

    auto get_state = [&type_envs](auto insn) -> const TypeEnvironment* {
      auto it = type_envs.find(insn);
      if (it == type_envs.end()) {
        return nullptr;
      }
      return &it->second;
    };

    for (const MethodItemEntry& mie : cfg::InstructionIterable(*cfg)) {
      auto insn = mie.insn;
      if (insn->opcode() != OPCODE_INSTANCE_OF) {
        continue;
      }

      auto state = get_state(insn);
      if (state == nullptr) {
        continue;
      }

      auto test_type = insn->get_type();

      auto src_type_state = state->get_dex_type(insn->src(0));
      if (!src_type_state) {
        continue;
      }

      auto eval = evaluate_impl(*src_type_state, test_type);
      if (!eval) {
        continue;
      }

      if (traceEnabled(EVALTC, 2)) {
        std::ostringstream oss;
        oss << "Found instance-of that can be evaluated: " << show(mie) << '\n';

        oss << "Test type:\n";
        print_type_chain(oss, test_type, 1);
        oss << "Source type:\n";
        print_type_chain(oss, *src_type_state, 1);
        oss << "Evaluates to:\n " << *eval;

        TRACE(EVALTC, 1, "%s", oss.str().c_str());
      }

      if (*eval == 1) {
        true_modulo_null_set.insert(&mie);
        ++res.class_always_succeed_or_null;
        continue;
      }
      redex_assert(*eval == 0);

      auto def_it = cfg->find_insn(insn);
      auto move_it = cfg->move_result_of(def_it);
      if (move_it.is_end()) { // Should not happen.
        continue;
      }
      reg_t trg_reg = move_it->insn->dest();

      // Schedule a bypass.
      auto set_result = new IRInstruction(OPCODE_CONST);
      set_result->set_dest(trg_reg);
      set_result->set_literal(0);
      mutation.insert_after(move_it, {set_result});

      ++res.overrides;
      ++res.class_always_fail;
    }
  }

  // See whether the checks that will succeed if the value is not null
  // can be turned into a null check. If the result is used for more
  // than a branch, transformation is likely not beneficial at the moment.
  analyze_true_instance_ofs(*cfg, mutation, res, true_modulo_null_set);

  mutation.flush();
  return res;
}

size_t post_process(DexMethod* method,
                    size_t overrides,
                    const XStoreRefs& xstores) {
  auto code = method->get_code();
  size_t num_insns_before = code->count_opcodes() - overrides;

  // Run ConstProp, CopyProp and DCE.
  {
    code->build_cfg(/*editable=*/false);

    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
      fp_iter.run(ConstantEnvironment());
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      tf.apply_on_uneditable_cfg(fp_iter,
                                 constant_propagation::WholeProgramState(),
                                 code, &xstores, method->get_class());
    }

    {
      copy_propagation_impl::Config copy_prop_config;
      copy_prop_config.eliminate_const_classes = false;
      copy_prop_config.eliminate_const_strings = false;
      copy_prop_config.static_finals = false;
      copy_propagation_impl::CopyPropagation copy_propagation(copy_prop_config);
      copy_propagation.run(code, method);
    }

    code->clear_cfg();
  }

  {
    ScopedCFG cfg(code);
    cfg->calculate_exit_block();
    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          *cfg, constant_propagation::ConstantPrimitiveAnalyzer());
      fp_iter.run(ConstantEnvironment());
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      tf.apply(fp_iter, code->cfg(), method, &xstores);
    }

    {
      std::unordered_set<DexMethodRef*> pure; // Don't assume anything;
      LocalDce dce(pure, /* no mog */ nullptr,
                   /*may_allocate_registers=*/false);
      dce.dce(method->get_code());
    }
  }

  size_t num_insns_after = code->count_opcodes();
  return num_insns_before - num_insns_after;
}

} // namespace

boost::optional<int32_t> EvaluateTypeChecksPass::evaluate(
    const DexType* src_type, const DexType* test_type) {
  return evaluate_impl(src_type, test_type);
}

void EvaluateTypeChecksPass::optimize(DexMethod* method, XStoreRefs& xstores) {
  auto local_res = analyze_and_evaluate(method);

  if (local_res.overrides != 0) {
    local_res.insn_delta = post_process(method, local_res.overrides, xstores);
  }
}

void EvaluateTypeChecksPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles&,
                                      PassManager& mgr) {
  auto scope = build_class_scope(stores);
  XStoreRefs xstores(stores);

  auto stats = walk::parallel::methods<RemoveResult>(
      scope, [&xstores](DexMethod* method) {
        auto code = method->get_code();
        if (code == nullptr || method->rstate.no_optimizations()) {
          return RemoveResult{};
        }
        auto has_instance_of = [&code]() {
          for (const auto& mie : *code) {
            if (mie.type != MFLOW_OPCODE) {
              continue;
            }
            if (mie.insn->opcode() != OPCODE_INSTANCE_OF) {
              continue;
            }
            return true;
          }
          return false;
        };
        if (!has_instance_of()) {
          return RemoveResult();
        }

        auto res = analyze_and_evaluate(method);
        res.methods_w_instanceof = 1;

        if (res.overrides != 0) {
          res.insn_delta = post_process(method, res.overrides, xstores);
        }

        return res;
      });

  mgr.set_metric("num_methods_w_instance_of", stats.methods_w_instanceof);
  mgr.set_metric("num_overrides", stats.overrides);
  mgr.set_metric("num_insn_delta", stats.insn_delta);
  mgr.set_metric("num_class_always_succeed_or_null",
                 stats.class_always_succeed_or_null);
  mgr.set_metric("num_class_always_succeed_or_null_repl",
                 stats.class_always_succeed_or_null_repl);
  mgr.set_metric("num_class_always_fail", stats.class_always_fail);
  mgr.set_metric("num_def_use_loop", stats.def_use_loop);
  mgr.set_metric("num_multi_use", stats.multi_use);
  mgr.set_metric("num_non_move", stats.non_move);
}

static EvaluateTypeChecksPass s_pass;

} // namespace check_casts
