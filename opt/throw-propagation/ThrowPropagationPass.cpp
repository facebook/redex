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
#include "Show.h"
#include "ThrowPropagationImpl.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

namespace {

constexpr const char* METRIC_THROWS_INSERTED = "num_throws_inserted";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTIONS =
    "num_unreachable_instructions";
constexpr const char* METRIC_NO_RETURN_METHODS = "num_no_return_methods";
constexpr const char* METRIC_ITERATIONS = "num_iterations";

bool exclude_method(DexMethod* method) {
  return method->get_code() == nullptr || is_abstract(method) ||
         method->is_external() || is_native(method) ||
         method->rstate.no_optimizations();
}

} // namespace

void ThrowPropagationPass::bind_config() {
  bind("debug", false, m_config.debug);
  bind("blocklist",
       {},
       m_config.blocklist,
       "List of classes that will not be analyzed to determine which methods "
       "have no return.");
}

bool ThrowPropagationPass::is_no_return_method(const Config& config,
                                               DexMethod* method) {
  if (exclude_method(method)) {
    return false;
  }
  if (config.blocklist.count(method->get_class())) {
    TRACE(TP, 4, "black-listed method: %s", SHOW(method));
    return false;
  }
  bool can_return{false};
  editable_cfg_adapter::iterate_with_iterator(
      method->get_code(), [&can_return](const IRList::iterator& it) {
        if (opcode::is_a_return(it->insn->opcode())) {
          can_return = true;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return !can_return;
}

ConcurrentSet<DexMethod*> ThrowPropagationPass::get_no_return_methods(
    const Config& config, const Scope& scope) {
  ConcurrentSet<DexMethod*> concurrent_no_return_methods;
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (is_no_return_method(config, method)) {
      concurrent_no_return_methods.insert(method);
    }
  });
  return concurrent_no_return_methods;
}

ThrowPropagationPass::Stats ThrowPropagationPass::run(
    const Config& config,
    const ConcurrentSet<DexMethod*>& no_return_methods,
    const method_override_graph::Graph& graph,
    IRCode* code,
    std::unordered_set<DexMethod*>* no_return_methods_checked) {
  ThrowPropagationPass::Stats stats;
  auto& cfg = code->cfg();
  std::vector<DexMethod*> return_methods;
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
    return_methods.clear();
    auto check_for_no_return = [&](DexMethod* other_method) {
      if (exclude_method(other_method)) {
        return false;
      }
      if (!no_return_methods.count_unsafe(other_method)) {
        return_methods.push_back(other_method);
      }
      return true;
    };
    if (!process_base_and_overriding_methods(
            &graph, method, /* methods_to_ignore */ nullptr,
            /* ignore_methods_with_assumenosideeffects */ false,
            check_for_no_return)) {
      return false;
    }
    if (no_return_methods_checked != nullptr) {
      // Tracking any of the return_methods is sufficient. We pick one in a
      // deterministic way.
      auto return_method_it = std::min_element(
          return_methods.begin(), return_methods.end(), compare_dexmethods);
      if (return_method_it != return_methods.end()) {
        no_return_methods_checked->insert(*return_method_it);
      }
    }
    return return_methods.empty();
  };

  throw_propagation_impl::ThrowPropagator impl(cfg, config.debug);
  for (auto block : cfg.blocks()) {
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto insn = it->insn;
      if (!is_no_return_invoke(insn)) {
        continue;
      }

      if (impl.try_apply(block->to_cfg_instruction_iterator(it))) {
        stats.throws_inserted++;
      }

      // Stop processing more instructions in this block
      break;
    }
  }

  if (stats.throws_inserted > 0) {
    stats.unreachable_instruction_count +=
        cfg.remove_unreachable_blocks().first;
    cfg.recompute_registers_size();
  }

  return stats;
}

void ThrowPropagationPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles&,
                                    PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  auto override_graph = method_override_graph::build_graph(scope);
  ConcurrentSet<DexMethod*> no_return_methods;
  {
    Timer t("get_no_return_methods");
    no_return_methods = get_no_return_methods(m_config, scope);
  }
  std::unordered_set<DexMethod*> impacted_methods;
  walk::code(scope, [&](DexMethod* method, IRCode&) {
    if (!method->rstate.no_optimizations()) {
      impacted_methods.insert(method);
    }
  });
  ConcurrentMap<DexMethod*, std::unordered_set<DexMethod*>> dependencies;
  std::unordered_set<DexMethod*> new_no_return_methods;
  std::mutex new_no_return_methods_mutex;
  Stats stats;
  int iterations = 0;
  while (!impacted_methods.empty()) {
    TRACE(TP, 2,
          "iteration %d, no_return_methods: %zu, impacted_methods: %zu, "
          "new_no_return_methods: %zu",
          iterations, no_return_methods.size(), impacted_methods.size(),
          new_no_return_methods.size());
    iterations++;
    std::mutex stats_mutex;
    new_no_return_methods.clear();
    workqueue_run<DexMethod*>(
        [&](DexMethod* method) {
          auto code = method->get_code();
          if (method->rstate.no_optimizations()) {
            return;
          }

          std::unordered_set<DexMethod*> no_return_methods_checked;
          auto local_stats = run(m_config,
                                 no_return_methods,
                                 *override_graph,
                                 code,
                                 &no_return_methods_checked);
          for (auto* other_method : no_return_methods_checked) {
            dependencies.update(other_method, [&](auto*, auto& set, bool) {
              set.insert(method);
            });
          }
          if (local_stats.throws_inserted > 0) {
            if (!no_return_methods.count_unsafe(method) &&
                is_no_return_method(m_config, method)) {
              std::lock_guard<std::mutex> lock_guard(
                  new_no_return_methods_mutex);
              new_no_return_methods.insert(method);
            }
            std::lock_guard<std::mutex> lock_guard(stats_mutex);
            stats += local_stats;
          }
        },
        impacted_methods);
    impacted_methods.clear();
    for (auto* method : new_no_return_methods) {
      auto it = dependencies.find(method);
      if (it != dependencies.end()) {
        impacted_methods.insert(it->second.begin(), it->second.end());
      }
      no_return_methods.insert(method);
    }
  }

  mgr.incr_metric(METRIC_THROWS_INSERTED, stats.throws_inserted);
  mgr.incr_metric(METRIC_UNREACHABLE_INSTRUCTIONS,
                  stats.unreachable_instruction_count);
  mgr.incr_metric(METRIC_NO_RETURN_METHODS, no_return_methods.size());
  mgr.incr_metric(METRIC_ITERATIONS, iterations);
}

ThrowPropagationPass::Stats& ThrowPropagationPass::Stats::operator+=(
    const Stats& that) {
  throws_inserted += that.throws_inserted;
  unreachable_instruction_count += that.unreachable_instruction_count;
  return *this;
}

static ThrowPropagationPass s_pass;
