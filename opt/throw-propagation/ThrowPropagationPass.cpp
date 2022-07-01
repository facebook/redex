/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This optimizer pass removes dead code by inserting throw instructions as
 * follows:
 *
 * When a method invocation is known to have no normal return behavior (because
 * all possibly invoked methods are known and have no normal return path, as
 * they either throw an exception or do not terminate, but never return), then
 * all instructions following such an invocation are dead.
 *
 * In such cases, we insert
 *   new-instance v0, Ljava/lang/RuntimeException;
 *   const-string v1, "Redex: Unreachable code after no-return invoke"
 *   invoke-direct v0, v1,
 * Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V throw v0 after such
 * invocations. The control-flow graph will then remove all no longer reachable
 * instructions and blocks. We run this to a fixed point.
 *
 * TODO: Run constant-propagation in caller, and then do callsite-specific
 * constant-propagation in callee (similar to what the inliner does); some
 * return instructions might turn out to be unreachable for particular
 * callsites, and thus invocations might more often be determined to not return.
 * (This could in many cases detect precondition violations, as
 * precondition-check methods typically conditionally throw/return, and then we
 * could effectively remove the entire method body. Cool optimization, but I
 * don't know how often it applies in practice...)
 * Then again, in another generalization, all this could one day be part of the
 * interprocedural constant-propagation.
 */

#include "ThrowPropagationPass.h"

#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "PassManager.h"
#include "Purity.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_THROWS_INSERTED = "num_throws_inserted";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTIONS =
    "num_unreachable_instructions";
constexpr const char* METRIC_NO_RETURN_METHODS = "num_no_return_methods";
constexpr const char* METRIC_ITERATIONS = "num_iterations";

} // namespace

void ThrowPropagationPass::bind_config() {
  bind("debug", false, m_config.debug);
  bind("blocklist",
       {},
       m_config.blocklist,
       "List of classes that will not be analyzed to determine which methods "
       "have no return.");
}

std::unordered_set<DexMethod*> ThrowPropagationPass::get_no_return_methods(
    const Config& config, const Scope& scope) {
  ConcurrentSet<DexMethod*> concurrent_no_return_methods;
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (is_abstract(method) || method->is_external() || is_native(method) ||
        method->rstate.no_optimizations()) {
      return;
    }
    if (config.blocklist.count(method->get_class())) {
      TRACE(TP, 4, "black-listed method: %s", SHOW(method));
      return;
    }
    bool can_return{false};
    editable_cfg_adapter::iterate_with_iterator(
        method->get_code(), [&can_return](const IRList::iterator& it) {
          if (opcode::is_a_return(it->insn->opcode())) {
            can_return = true;
            return editable_cfg_adapter::LOOP_BREAK;
          } else {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
        });
    if (!can_return) {
      concurrent_no_return_methods.insert(method);
    }
  });
  std::unordered_set<DexMethod*> no_return_methods(
      concurrent_no_return_methods.begin(), concurrent_no_return_methods.end());
  return no_return_methods;
}

ThrowPropagationPass::Stats ThrowPropagationPass::run(
    const Config& config,
    const std::unordered_set<DexMethod*>& no_return_methods,
    const method_override_graph::Graph& graph,
    IRCode* code) {
  ThrowPropagationPass::Stats stats;
  cfg::ScopedCFG cfg(code);
  auto is_no_return_invoke = [&](IRInstruction* insn) {
    if (!opcode::is_an_invoke(insn->opcode())) {
      return false;
    }
    if (insn->opcode() == OPCODE_INVOKE_SUPER) {
      // TODO
      return false;
    }
    auto method_ref = insn->get_method();
    DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
    if (method == nullptr) {
      return false;
    }
    if (insn->opcode() == OPCODE_INVOKE_INTERFACE &&
        is_annotation(type_class(method->get_class()))) {
      TRACE(TP, 4, "annotation interface method: %s", SHOW(method));
      return false;
    }
    bool invoke_can_return{false};
    auto check_for_no_return = [&](DexMethod* other_method) {
      if (!no_return_methods.count(other_method)) {
        invoke_can_return = true;
      }
      return true;
    };
    if (!process_base_and_overriding_methods(
            &graph, method, /* methods_to_ignore */ nullptr,
            /* ignore_methods_with_assumenosideeffects */ false,
            check_for_no_return)) {
      return false;
    }
    return !invoke_can_return;
  };

  boost::optional<std::pair<reg_t, reg_t>> regs;
  auto will_throw_or_not_terminate = [&cfg](cfg::InstructionIterator it) {
    std::unordered_set<IRInstruction*> visited{it->insn};
    while (true) {
      it = cfg->next_following_gotos(it);
      if (!visited.insert(it->insn).second) {
        // We found a loop
        return true;
      }
      switch (it->insn->opcode()) {
      case OPCODE_CONST:
      case OPCODE_CONST_STRING:
      case OPCODE_MOVE:
      case OPCODE_NOP:
      case OPCODE_NEW_INSTANCE:
      case IOPCODE_LOAD_PARAM:
      case IOPCODE_LOAD_PARAM_OBJECT:
      case IOPCODE_MOVE_RESULT_PSEUDO:
        break;
      case OPCODE_INVOKE_DIRECT: {
        auto method = it->insn->get_method();
        if (!method::is_init(method) ||
            method->get_class() !=
                DexType::make_type("Ljava/lang/RuntimeException;")) {
          return false;
        }
        break;
      }
      case OPCODE_THROW:
        return true;
      default:
        return false;
      }
    }
    not_reached();
  };
  // Helper function that checks if there's any point in doing a transformation
  // (not needed if we are already going to throw or not terminate anyway),
  // and it performs block splitting if needed (see comment inline for details).
  auto check_if_dead_code_present_and_prepare_block =
      [&](cfg::Block* block, const ir_list::InstructionIterator& it) -> bool {
    auto insn = it->insn;
    TRACE(TP, 4, "no return: %s", SHOW(insn));
    auto cfg_it = block->to_cfg_instruction_iterator(it);
    if (insn == block->get_last_insn()->insn) {
      if (will_throw_or_not_terminate(cfg_it)) {
        // There's already code in place that will immediately and
        // unconditionally throw an exception, and thus we don't need to
        // bother rewriting the code into a throw. The main reason we are
        // doing this is to not inflate our throws_inserted statistics.
        return false;
      }
    } else {
      // When the invoke instruction isn't the last in the block, then we'll
      // need to some extra work. (Ideally, we could have just inserted our
      // throwing instructions in the middle of the existing block, but that
      // causes complications due to the possibly following and then dangling
      // move-result instruction, so we'll explicitly split the block here
      // in order to keep all invariant happy.)
      if (will_throw_or_not_terminate(cfg_it)) {
        // As above, nothing to do, since an exception will be thrown anyway.
        return false;
      }
      always_assert(cfg->get_succ_edge_of_type(block, cfg::EDGE_THROW) ==
                    nullptr);
      cfg->split_block(cfg_it);
      always_assert(insn == block->get_last_insn()->insn);
    }
    return true;
  };
  auto insert_throw = [&](cfg::Block* block, IRInstruction* insn) {
    std::string message{"Redex: Unreachable code after no-return invoke"};
    if (config.debug) {
      message += ":";
      message += SHOW(insn);
    }
    if (!regs) {
      regs = std::make_pair(cfg->allocate_temp(), cfg->allocate_temp());
    }
    auto exception_reg = regs->first;
    auto string_reg = regs->second;
    cfg::Block* new_block = cfg->create_block();
    std::vector<IRInstruction*> insns;
    auto new_instance_insn = new IRInstruction(OPCODE_NEW_INSTANCE);
    auto exception_type = DexType::get_type("Ljava/lang/RuntimeException;");
    always_assert(exception_type != nullptr);
    new_instance_insn->set_type(exception_type);
    insns.push_back(new_instance_insn);

    auto move_result_pseudo_exception_insn =
        new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
    move_result_pseudo_exception_insn->set_dest(exception_reg);
    insns.push_back(move_result_pseudo_exception_insn);

    auto const_string_insn = new IRInstruction(OPCODE_CONST_STRING);
    const_string_insn->set_string(DexString::make_string(message));
    insns.push_back(const_string_insn);

    auto move_result_pseudo_string_insn =
        new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
    move_result_pseudo_string_insn->set_dest(string_reg);
    insns.push_back(move_result_pseudo_string_insn);

    auto invoke_direct_insn = new IRInstruction(OPCODE_INVOKE_DIRECT);
    auto init_method = DexMethod::get_method(
        "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V");
    always_assert(init_method != nullptr);
    invoke_direct_insn->set_method(init_method)
        ->set_srcs_size(2)
        ->set_src(0, exception_reg)
        ->set_src(1, string_reg);
    insns.push_back(invoke_direct_insn);
    auto throw_insn = new IRInstruction(OPCODE_THROW);
    throw_insn->set_src(0, exception_reg);
    insns.push_back(throw_insn);
    new_block->push_back(insns);
    cfg->copy_succ_edges_of_type(block, new_block, cfg::EDGE_THROW);
    auto existing_goto_edge = cfg->get_succ_edge_of_type(block, cfg::EDGE_GOTO);
    always_assert(existing_goto_edge != nullptr);
    cfg->set_edge_target(existing_goto_edge, new_block);
    stats.throws_inserted++;
  };
  for (auto block : cfg->blocks()) {
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto insn = it->insn;
      if (!is_no_return_invoke(insn)) {
        continue;
      }

      if (!check_if_dead_code_present_and_prepare_block(block, it)) {
        continue;
      }

      insert_throw(block, insn);

      // Stop processing more instructions in this block
      break;
    }
  }

  if (stats.throws_inserted > 0) {
    stats.unreachable_instruction_count +=
        cfg->remove_unreachable_blocks().first;
    cfg->recompute_registers_size();
  }

  return stats;
}

void ThrowPropagationPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles&,
                                    PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    if (!method->rstate.no_optimizations()) {
      code.build_cfg(/* editable */ true);
    }
  });
  auto override_graph = method_override_graph::build_graph(scope);
  size_t last_no_return_methods{0};
  int iterations = 0;
  Stats stats;
  while (true) {
    iterations++;
    std::unordered_set<DexMethod*> no_return_methods =
        get_no_return_methods(m_config, scope);
    TRACE(TP,
          2,
          "iteration %d, no_return_methods: %zu",
          iterations,
          no_return_methods.size());
    if (no_return_methods.size() == last_no_return_methods) {
      break;
    }
    last_no_return_methods = no_return_methods.size();
    auto last_stats =
        walk::parallel::methods<Stats>(scope, [&](DexMethod* method) -> Stats {
          auto code = method->get_code();
          if (method->rstate.no_optimizations() || code == nullptr) {
            return {};
          }

          return run(m_config, no_return_methods, *override_graph, code);
        });
    if (last_stats.throws_inserted == 0) {
      break;
    }
    stats += last_stats;
  }

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    if (!method->rstate.no_optimizations()) {
      code.clear_cfg();
    }
  });

  mgr.incr_metric(METRIC_THROWS_INSERTED, stats.throws_inserted);
  mgr.incr_metric(METRIC_UNREACHABLE_INSTRUCTIONS,
                  stats.unreachable_instruction_count);
  mgr.incr_metric(METRIC_NO_RETURN_METHODS, last_no_return_methods);
  mgr.incr_metric(METRIC_ITERATIONS, iterations);
}

ThrowPropagationPass::Stats& ThrowPropagationPass::Stats::operator+=(
    const Stats& that) {
  throws_inserted += that.throws_inserted;
  unreachable_instruction_count += that.unreachable_instruction_count;
  return *this;
}

static ThrowPropagationPass s_pass;
