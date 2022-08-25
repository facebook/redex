/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <vector>

#include "ABExperimentContext.h"
#include "CallSiteSummaries.h"
#include "PriorityThreadPoolDAGScheduler.h"
#include "RefChecker.h"
#include "Resolver.h"
#include "Shrinker.h"

class InlineForSpeed;

namespace inliner {

struct InlinerConfig;

/*
 * Use the editable CFG instead of IRCode to do the inlining. Return true on
 * success. Registers starting with next_caller_reg must be available
 */
bool inline_with_cfg(DexMethod* caller_method,
                     DexMethod* callee_method,
                     IRInstruction* callsite,
                     DexType* needs_receiver_cast,
                     DexType* needs_init_class,
                     size_t next_caller_reg,
                     const cfg::ControlFlowGraph* reduced_cfg = nullptr);

} // namespace inliner

/**
 * What kind of caller-callee relationships the inliner should consider.
 */
enum MultiMethodInlinerMode {
  None,
  InterDex,
  IntraDex,
};

// All call-sites of a callee.
struct CallerInsns {
  // Invoke instructions per caller
  std::unordered_map<const DexMethod*, std::unordered_set<IRInstruction*>>
      caller_insns;
  // Invoke instructions that need a cast
  std::unordered_map<IRInstruction*, DexType*> inlined_invokes_need_cast;
  // Whether there may be any other unknown call-sites.
  bool other_call_sites{false};
  bool empty() const { return caller_insns.empty() && !other_call_sites; }
};

using CalleeCallerInsns = std::unordered_map<DexMethod*, CallerInsns>;

class ReducedCode {
 public:
  ReducedCode() : m_code(std::make_unique<cfg::ControlFlowGraph>()) {}
  IRCode& code() { return m_code; }
  cfg::ControlFlowGraph& cfg() { return m_code.cfg(); }

 private:
  IRCode m_code;
};

struct Inlinable {
  DexMethod* callee;
  // Only used when not using cfg; iterator to invoke instruction to callee
  IRList::iterator iterator;
  // Invoke instruction to callee
  IRInstruction* insn;
  // Whether the invocation at a particular call-site is guaranteed to not
  // return normally, and instead of inlining, a throw statement should be
  // inserted afterwards.
  bool no_return{false};
  // For a specific call-site, reduced cfg template after applying call-site
  // summary
  std::shared_ptr<ReducedCode> reduced_code;
  // Estimated size of callee, possibly reduced by call-site specific knowledge
  size_t insn_size;
};

struct CalleeCallerRefs {
  bool same_class;
  size_t classes;
};

// The average or call-site specific inlined costs, depending on how it is
// retrieved
struct InlinedCost {
  // Full code costs of the original callee
  size_t full_code;
  // Average or call-site specific code costs of the callee after pruning
  float code;
  // Average or call-site specific method-refs count of the callee after pruning
  float method_refs;
  // Average or call-site specific others-refs count of the callee after pruning
  float other_refs;
  // Whether all or a specific call-site is guaranteed to not return normally
  bool no_return;
  // Average or call-site specific value indicating whether result is used
  float result_used;
  // Average or call-site specific value indicating how many callee arguments
  // are unused
  float unused_args;
  // For a specific call-site, reduced cfg template after applying call-site
  // summary
  std::shared_ptr<ReducedCode> reduced_code;
  // Maximum or call-site specific estimated callee size after pruning
  size_t insn_size;

  bool operator==(const InlinedCost& other) {
    // TODO: Also check that reduced_cfg's are equivalent
    return full_code == other.full_code && code == other.code &&
           method_refs == other.method_refs && other_refs == other.other_refs &&
           no_return == other.no_return && result_used == other.result_used &&
           insn_size == other.insn_size;
  }
};

/**
 * Helper class to inline a set of candidates.
 * Take a set of candidates and a scope and walk all instructions in scope
 * to find and inline all calls to candidate.
 * A resolver is used to map a method reference to a method definition, and must
 * be thread-safe. Not all methods may be inlined both for restriction on the
 * caller or the callee. Perform inlining bottom up.
 */
class MultiMethodInliner {
 public:
  /**
   * We can do global inlining before InterDexPass, but after InterDexPass, we
   * can only inline methods within each dex. Set intra_dex to true if
   * inlining is needed after InterDex.
   */
  MultiMethodInliner(
      const std::vector<DexClass*>& scope,
      const init_classes::InitClassesWithSideEffects&
          init_classes_with_side_effects,
      DexStoresVector& stores,
      const std::unordered_set<DexMethod*>& candidates,
      std::function<DexMethod*(DexMethodRef*, MethodSearch)>
          concurrent_resolve_fn,
      const inliner::InlinerConfig& config,
      int min_sdk,
      MultiMethodInlinerMode mode = InterDex,
      const CalleeCallerInsns& true_virtual_callers = {},
      InlineForSpeed* inline_for_speed = nullptr,
      bool analyze_and_prune_inits = false,
      const std::unordered_set<DexMethodRef*>& configured_pure_methods = {},
      const api::AndroidSDK* min_sdk_api = nullptr,
      bool cross_dex_penalty = false,
      const std::unordered_set<const DexString*>&
          configured_finalish_field_names = {});

  ~MultiMethodInliner() { delayed_invoke_direct_to_static(); }

  /**
   * attempt inlining for all candidates.
   */
  void inline_methods(bool methods_need_deconstruct = true);

  /**
   * Return the set of unique inlined methods.
   */
  std::unordered_set<DexMethod*> get_inlined() const {
    std::unordered_set<DexMethod*> res(m_inlined.begin(), m_inlined.end());
    return res;
  }

  bool for_speed() const { return m_inline_for_speed != nullptr; }

  /**
   * Inline callees in the caller if is_inlinable below returns true.
   */
  void inline_callees(DexMethod* caller,
                      const std::unordered_set<DexMethod*>& callees,
                      bool filter_via_should_inline = false);

  /**
   * Inline callees in the given instructions in the caller, if is_inlinable
   * below returns true.
   */
  size_t inline_callees(DexMethod* caller,
                        const std::unordered_set<IRInstruction*>& insns,
                        std::vector<IRInstruction*>* deleted_insns = nullptr);

  /**
   * Return true if the callee is inlinable into the caller.
   * The predicates below define the constraints for inlining.
   * Providing an instrucion is optional, and only used for logging.
   */
  bool is_inlinable(const DexMethod* caller,
                    const DexMethod* callee,
                    const IRInstruction* insn,
                    uint64_t estimated_caller_size,
                    uint64_t estimated_callee_size,
                    bool* caller_too_large_ = nullptr);

  void visibility_changes_apply_and_record_make_static(
      const VisibilityChanges& visibility_changes);

  shrinker::Shrinker& get_shrinker() { return m_shrinker; }

 private:
  DexType* get_needs_init_class(DexMethod* callee) const;

  DexMethod* get_callee(DexMethod* caller, IRInstruction* insn);

  size_t inline_inlinables(
      DexMethod* caller,
      const std::vector<Inlinable>& inlinables,
      std::vector<IRInstruction*>* deleted_insns = nullptr);

  /**
   * Return true if the method is related to enum (java.lang.Enum and derived).
   * Cannot inline enum methods because they can be called by code we do
   * not own.
   */
  bool is_blocklisted(const DexMethod* callee);

  bool caller_is_blocklisted(const DexMethod* caller);

  /**
   * Return true if the callee contains external catch exception types
   * which are not public.
   */
  bool has_external_catch(const DexMethod* callee);

  /**
   * Return true if the callee contains certain opcodes that are difficult
   * or impossible to inline.
   * Some of the opcodes are defined by the methods below.
   */
  bool cannot_inline_opcodes(const DexMethod* caller,
                             const DexMethod* callee,
                             const IRInstruction* invk_insn);

  /**
   * Return true if inlining would require a method called from the callee
   * (candidate) to turn into a virtual method (e.g. private to public).
   */
  bool create_vmethod(IRInstruction* insn,
                      const DexMethod* callee,
                      const DexMethod* caller);

  /**
   * Return true if we would create an invocation within an outlined method to
   * another outlined method.
   */
  bool outlined_invoke_outlined(IRInstruction* insn, const DexMethod* caller);

  /**
   * Return true if a callee contains an invoke super to a different method
   * in the hierarchy.
   * invoke-super can only exist within the class the call lives in.
   */
  bool nonrelocatable_invoke_super(IRInstruction* insn);

  /**
   * Return true if the callee contains a call to an unknown virtual method.
   * We cannot determine the visibility of the method invoked and thus
   * we cannot inline as we could cause a verification error if the method
   * was package/protected and we move the call out of context.
   */
  bool unknown_virtual(IRInstruction* insn);

  /**
   * Return true if the callee contains an access to an unknown field.
   * We cannot determine the visibility of the field accessed and thus
   * we cannot inline as we could cause a verification error if the field
   * was package/protected and we move the access out of context.
   */
  bool unknown_field(IRInstruction* insn);

  /**
   * return true if `insn` is
   *   sget android.os.Build.VERSION.SDK_INT
   */
  bool check_android_os_version(IRInstruction* insn);

  /**
   * Return true if a caller is in a DEX in a store and any opcode in callee
   * refers to a DexMember in a different store .
   */
  bool cross_store_reference(const DexMethod* caller, const DexMethod* callee);

  /**
   * Return true if a caller is in a DEX in a store and any opcode in callee
   * refers to a problematic ref, i.e. one that directly or indirectly refers to
   * another store, or a non-min-sdk API.
   */
  bool problematic_refs(const DexMethod* caller, const DexMethod* callee);

  bool is_estimate_over_max(uint64_t estimated_caller_size,
                            uint64_t estimated_callee_size,
                            uint64_t max);

  /**
   * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
   * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
   *
   * Right now we only check for the number of instructions, but there are
   * other constraints that might be worth looking into, e.g. the number of
   * registers.
   */
  bool caller_too_large(DexType* caller_type,
                        uint64_t estimated_caller_size,
                        uint64_t estimated_callee_size);

  /**
   * Return whether the callee should be inlined into the caller. This differs
   * from is_inlinable in that the former is concerned with whether inlining is
   * possible to do correctly at all, whereas this is concerned with whether the
   * inlining is beneficial for size / performance.
   *
   * This method does *not* need to return a subset of is_inlinable. We will
   * only inline callsites that pass both should_inline and is_inlinable.
   *
   * Note that this filter will only be applied when inlining is initiated via
   * a call to `inline_methods()`, but not if `inline_callees()` is invoked
   * directly.
   */
  bool should_inline_always(const DexMethod* callee);

  /**
   * Whether it's beneficial to inline the callee at a particular callsite.
   * no_return may be set to true when the return value is false.
   * reduced_cfg and insn_size are set when the return value is true.
   */
  bool should_inline_at_call_site(
      DexMethod* caller,
      const IRInstruction* invoke_insn,
      DexMethod* callee,
      bool* no_return = nullptr,
      std::shared_ptr<ReducedCode>* reduced_code = nullptr,
      size_t* insn_size = nullptr);

  /**
   * should_inline_fast will return true for a subset of methods compared to
   * should_inline. should_inline_fast can be evaluated much more quickly, as it
   * doesn't need to peek into the callee code.
   */
  bool should_inline_fast(const DexMethod* callee);

  /**
   * Gets the number of instructions in a callee.
   */
  size_t get_callee_insn_size(const DexMethod* callee);

  /**
   * Gets the set of referenced types in a callee.
   */
  std::shared_ptr<std::vector<DexType*>> get_callee_type_refs(
      const DexMethod* callee);

  /**
   * Gets the set of references in a callee's code.
   */
  std::shared_ptr<CodeRefs> get_callee_code_refs(const DexMethod* callee);

  /**
   * Computes information about callers of a method.
   */
  CalleeCallerRefs get_callee_caller_refs(const DexMethod* callee);

  /**
   * We want to avoid inlining a large method with many callers as that would
   * bloat the bytecode.
   */
  bool too_many_callers(const DexMethod* callee);

  // Reduce a cfg with a call-site summary, if given.
  std::shared_ptr<ReducedCode> apply_call_site_summary(
      bool is_static,
      DexType* declaring_type,
      DexProto* proto,
      const cfg::ControlFlowGraph& original_cfg,
      const CallSiteSummary* call_site_summary);

  /*
   * Try to estimate number of code units (2 bytes each) of code. Also take
   * into account costs arising from control-flow overhead and constant
   * arguments, if any
   */
  InlinedCost get_inlined_cost(
      bool is_static,
      DexType* declaring_type,
      DexProto* proto,
      const IRCode* code,
      const CallSiteSummary* call_site_summary = nullptr);

  /**
   * Estimate inlined cost for fully inlining a callee without using any
   * summaries for pruning.
   */
  const InlinedCost* get_fully_inlined_cost(const DexMethod* callee);

  /**
   * Estimate average inlined cost when inlining a callee, considering all
   * call-site summaries for pruning.
   */
  const InlinedCost* get_average_inlined_cost(const DexMethod* callee);

  /**
   * Estimate inlined cost for a particular call-site, if available.
   */
  const InlinedCost* get_call_site_inlined_cost(
      const IRInstruction* invoke_insn, const DexMethod* callee);

  /**
   * Estimate inlined cost for a particular call-site summary, if available.
   */
  const InlinedCost* get_call_site_inlined_cost(
      const CallSiteSummary* call_site_summary, const DexMethod* callee);

  /**
   * Change visibilities of methods, assuming that`m_visibility_changes` is
   * non-null.
   */
  void delayed_visibility_changes_apply();

  /**
   * Staticize required methods (stored in `m_delayed_make_static`) and update
   * opcodes accordingly.
   *
   * NOTE: It only needs to be called once after inlining. Since it is called
   *       from the destructor, there is no need to manually call it.
   */
  void delayed_invoke_direct_to_static();

  /**
   * Initiate computation of various callee costs asynchronously.
   */
  void compute_callee_costs(DexMethod* method);

  /**
   * Post-processing a method synchronously.
   */
  void postprocess_method(DexMethod* method);

  /**
   * Shrink a method (run constant-prop, cse, copy-prop, local-dce,
   * dedup-blocks) synchronously.
   */
  void shrink_method(DexMethod* method);

  /**
   * Whether inline_inlinables needs to deconstruct the caller's and callees'
   * code.
   */
  bool inline_inlinables_need_deconstruct(DexMethod* method);

  // Checks that...
  // - there are no assignments to (non-inherited) instance fields before
  //   a constructor call, and
  // - the constructor refers to a method of the same class, and
  // - there are no assignments to any final fields.
  // Under these conditions, a constructor is universally inlinable.
  bool can_inline_init(const DexMethod* init_method);

 private:
  std::unique_ptr<std::vector<std::unique_ptr<RefChecker>>> m_ref_checkers;

  /**
   * Resolver function to map a method reference to a method definition. Must be
   * thread-safe.
   */
  std::function<DexMethod*(DexMethodRef*, MethodSearch)> m_concurrent_resolver;

  /**
   * Inlined methods.
   */
  ConcurrentSet<DexMethod*> m_inlined;

  //
  // Maps from callee to callers and reverse map from caller to callees.
  // Those are used to perform bottom up inlining.
  //
  MethodToMethodOccurrences callee_caller;

  MethodToMethodOccurrences caller_callee;

  // Auxiliary data for a caller that contains true virtual callees
  struct CallerVirtualCallees {
    // Mapping of instructions to representative
    std::unordered_map<IRInstruction*, DexMethod*> insns;
    // Set of callees which must only be inlined via above insns
    std::unordered_set<DexMethod*> exclusive_callees;
  };
  // Mapping from callers to auxiliary data for contained true virtual callees
  std::unordered_map<const DexMethod*, CallerVirtualCallees>
      m_caller_virtual_callees;

  std::unordered_map<IRInstruction*, DexType*> m_inlined_invokes_need_cast;

  std::unordered_set<const DexMethod*>
      m_true_virtual_callees_with_other_call_sites;

  std::unordered_set<const DexMethod*> m_recursive_callees;
  std::unordered_set<const DexMethod*> m_speed_excluded_callees;

  // If mode == IntraDex, then this is the set of callees that is reachable via
  // an (otherwise ignored) invocation from a caller in a different dex. If mode
  // != IntraDex, then the set is empty.
  std::unordered_set<const DexMethod*> m_x_dex_callees;

  // Cache of the inlined costs of fully inlining a calle without using any
  // summaries for pruning.
  mutable ConcurrentMap<const DexMethod*, std::shared_ptr<InlinedCost>>
      m_fully_inlined_costs;

  // Cache of the average inlined costs of each method.
  mutable ConcurrentMap<const DexMethod*, std::shared_ptr<InlinedCost>>
      m_average_inlined_costs;

  // Cache of the inlined costs of each call-site summary after pruning.
  mutable ConcurrentMap<CalleeCallSiteSummary,
                        std::shared_ptr<InlinedCost>,
                        boost::hash<CalleeCallSiteSummary>>
      m_call_site_inlined_costs;

  // Cache of the inlined costs of each call-site after pruning.
  mutable ConcurrentMap<const IRInstruction*,
                        boost::optional<const InlinedCost*>>
      m_invoke_call_site_inlined_costs;

  // Priority thread pool to handle parallel processing of methods, either
  // shrinking initially / after inlining into them, or even to inline in
  // parallel. By default, parallelism is disabled num_threads = 0).
  PriorityThreadPoolDAGScheduler<DexMethod*> m_scheduler;

  // Set of methods that need to be made static eventually. The destructor
  // of this class will do the necessary delayed work.
  ConcurrentSet<DexMethod*> m_delayed_make_static;

  // Accumulated visibility changes that must be applied eventually.
  // This happens locally within inline_methods.
  std::unique_ptr<VisibilityChanges> m_delayed_visibility_changes;

  // When mutating m_delayed_visibility_changes or applying visibility changes
  // eagerly
  std::mutex m_visibility_changes_mutex;

  // Cache for should_inline function
  ConcurrentMap<const DexMethod*, boost::optional<bool>> m_should_inline;

  // Optional cache for get_callee_insn_size function
  std::unique_ptr<ConcurrentMap<const DexMethod*, size_t>> m_callee_insn_sizes;

  // Optional cache for get_callee_type_refs function
  std::unique_ptr<
      ConcurrentMap<const DexMethod*, std::shared_ptr<std::vector<DexType*>>>>
      m_callee_type_refs;

  // Optional cache for get_callee_code_refs function
  std::unique_ptr<ConcurrentMap<const DexMethod*, std::shared_ptr<CodeRefs>>>
      m_callee_code_refs;

  // Optional cache for get_callee_caller_res function
  std::unique_ptr<ConcurrentMap<const DexMethod*, CalleeCallerRefs>>
      m_callee_caller_refs;

  // Cache of whether a constructor can be unconditionally inlined.
  mutable ConcurrentMap<const DexMethod*, boost::optional<bool>>
      m_can_inline_init;

 private:
  std::unique_ptr<inliner::CallSiteSummarizer> m_call_site_summarizer;

  /**
   * Info about inlining.
   */
  struct InliningInfo {
    // statistics that must be incremented sequentially
    size_t recursive{0};
    size_t max_call_stack_depth{0};
    size_t waited_seconds{0};
    int critical_path_length{0};

    // statistics that may be incremented concurrently
    std::atomic<size_t> kotlin_lambda_inlined{0};
    std::atomic<size_t> calls_inlined{0};
    std::atomic<size_t> init_classes{0};
    std::atomic<size_t> calls_not_inlinable{0};
    std::atomic<size_t> calls_not_inlined{0};
    std::atomic<size_t> no_returns{0};
    std::atomic<size_t> unreachable_insns{0};
    std::atomic<size_t> intermediate_shrinkings{0};
    std::atomic<size_t> intermediate_remove_unreachable_blocks{0};
    std::atomic<size_t> not_found{0};
    std::atomic<size_t> blocklisted{0};
    std::atomic<size_t> throws{0};
    std::atomic<size_t> multi_ret{0};
    std::atomic<size_t> need_vmethod{0};
    std::atomic<size_t> invoke_super{0};
    std::atomic<size_t> escaped_virtual{0};
    std::atomic<size_t> known_public_methods{0};
    std::atomic<size_t> unresolved_methods{0};
    std::atomic<size_t> non_pub_virtual{0};
    std::atomic<size_t> escaped_field{0};
    std::atomic<size_t> non_pub_field{0};
    std::atomic<size_t> non_pub_ctor{0};
    std::atomic<size_t> cross_store{0};
    std::atomic<size_t> api_level_mismatch{0};
    std::atomic<size_t> problematic_refs{0};
    std::atomic<size_t> caller_too_large{0};
    std::atomic<size_t> constant_invoke_callees_analyzed{0};
    std::atomic<size_t> constant_invoke_callees_unused_results{0};
    std::atomic<size_t> constant_invoke_callees_no_return{0};
    inliner::CallSiteSummaryStats call_site_summary_stats;
  };
  InliningInfo info;

  const std::vector<DexClass*>& m_scope;

  const inliner::InlinerConfig& m_config;

  const MultiMethodInlinerMode m_mode;

  // Non-const to allow for caching behavior.
  InlineForSpeed* m_inline_for_speed;

  // Whether to do some deep analysis to determine if constructor candidates
  // can be safely inlined, and don't inline them otherwise.
  bool m_analyze_and_prune_inits;

  bool m_cross_dex_penalty;

  shrinker::Shrinker m_shrinker;

  AccumulatingTimer m_inline_callees_timer;
  AccumulatingTimer m_inline_callees_should_inline_timer;
  AccumulatingTimer m_inline_callees_init_timer;
  AccumulatingTimer m_inline_inlinables_timer;
  AccumulatingTimer m_inline_with_cfg_timer;
  AccumulatingTimer m_call_site_inlined_cost_timer;
  AccumulatingTimer m_cannot_inline_sketchy_code_timer;

  std::unique_ptr<ab_test::ABExperimentContext> m_ab_experiment_context{
      nullptr};
  std::mutex ab_exp_mutex;

  const DexFieldRef* m_sdk_int_field =
      DexField::get_field("Landroid/os/Build$VERSION;.SDK_INT:I");

 public:
  const InliningInfo& get_info() { return info; }

  size_t get_callers() { return caller_callee.size(); }

  size_t get_x_dex_callees() { return m_x_dex_callees.size(); }

  double get_call_site_inlined_cost_seconds() const {
    return m_call_site_inlined_cost_timer.get_seconds();
  }
  double get_inline_callees_seconds() const {
    return m_inline_callees_timer.get_seconds() -
           m_inline_callees_should_inline_timer.get_seconds() -
           m_inline_callees_init_timer.get_seconds();
  }
  double get_inline_callees_should_inline_seconds() const {
    return m_inline_callees_should_inline_timer.get_seconds();
  }
  double get_inline_callees_init_seconds() const {
    return m_inline_callees_init_timer.get_seconds();
  }
  double get_inline_inlinables_seconds() const {
    return m_inline_inlinables_timer.get_seconds() -
           m_inline_with_cfg_timer.get_seconds();
  }
  double get_inline_with_cfg_seconds() const {
    return m_inline_with_cfg_timer.get_seconds();
  }
  double get_cannot_inline_sketchy_code_timer_seconds() const {
    return m_cannot_inline_sketchy_code_timer.get_seconds();
  }
};
