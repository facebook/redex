/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * VirtualMergingPass removes virtual methods that override other virtual
 * methods, by merging them, under certain conditions.
 * - we omit virtual scopes that are involved in invoke-supers (this could be
 *   made less conservative)
 * - we omit virtual methods that might be involved in unresolved
 *   invoke-virtuals.
 * - of, course the usual `can_rename` and not `root` conditions.
 * - the overriding method must be inlinable into the overridden method (using
 *   standard inliner functionality)
 *
 * When overriding an abstract method, the body of the overriding method is
 * essentially just moved into the formerly abstract method, with a preceeding
 * cast-class instruction to make the type checker happy. (The actual
 * implementation is a special case of the below, using the inliner.)
 *
 * When overriding a non-abstract method, we first insert a prologue like the
 * following into the overridden method:
 *
 * instance-of               param0, DeclaringTypeOfOverridingMethod
 * move-result-pseudo        if_temp
 * if-nez                    if_temp, new_code
 * ... (old body)
 *
 * new_code:
 * cast-class                param0, DeclaringTypeOfOverridingMethod
 * move-result-pseudo-object temp
 * invoke-virtual            temp, param1, ..., paramN, OverridingMethod
 * move-result               result_temp
 * return                    result_temp
 *
 * And then we inline the invoke-virtual instruction. Details vary depending on
 * the whether the method actually has a result, and if so, what kind it is.
 */

#include "VirtualMerging.h"

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "CppUtil.h"
#include "DedupVirtualMethods.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Inliner.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Resolver.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_DEDUPPED_VIRTUAL_METHODS =
    "num_dedupped_virtual_methods";
constexpr const char* METRIC_INVOKE_SUPER_METHODS = "num_invoke_super_methods";
constexpr const char* METRIC_INVOKE_SUPER_UNRESOLVED_METHOD_REFS =
    "num_invoke_super_unresolved_methods_refs";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_SCOPES =
    "num_mergeable_virtual_scopes";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS =
    "num_mergeable_virtual_methods";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS_ANNOTATED_METHODS =
    "num_mergeable_virtual_method_annotated_methods";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_STORE_REFS =
    "num_mergeable_virtual_method_cross_store_refs";
constexpr const char* METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_DEX_REFS =
    "num_mergeable_virtual_method_cross_dex_refs";
constexpr const char*
    METRIC_MERGEABLE_VIRTUAL_METHODS_INCONCRETE_OVERRIDDEN_METHODS =
        "num_mergeable_virtual_methods_inconcrete_overridden_methods";

constexpr const char* METRIC_MERGEABLE_PAIRS = "num_mergeable_pairs";
constexpr const char* METRIC_VIRTUAL_SCOPES_WITH_MERGEABLE_PAIRS =
    "num_virtual_scopes_with_mergeable_pairs";
constexpr const char* METRIC_UNABSTRACTED_METHODS = "num_unabstracted_methods";
constexpr const char* METRIC_UNINLINABLE_METHODS = "num_uninlinable_methods";
constexpr const char* METRIC_HUGE_METHODS = "num_huge_methods";
constexpr const char* METRIC_REMOVED_VIRTUAL_METHODS =
    "num_removed_virtual_methods";

} // namespace

VirtualMerging::VirtualMerging(DexStoresVector& stores,
                               const inliner::InlinerConfig& inliner_config,
                               size_t max_overriding_method_instructions)
    : m_scope(build_class_scope(stores)),
      m_xstores(stores),
      m_xdexes(stores),
      m_type_system(m_scope),
      m_max_overriding_method_instructions(max_overriding_method_instructions),
      m_inliner_config(inliner_config) {
  auto resolver = [&](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, m_resolved_refs);
  };

  std::unordered_set<DexMethod*> no_default_inlinables;
  m_inliner_config.use_cfg_inliner = true;
  m_inliner.reset(new MultiMethodInliner(m_scope, stores, no_default_inlinables,
                                         resolver, m_inliner_config,
                                         MultiMethodInlinerMode::None));
}
VirtualMerging::~VirtualMerging() {}

// Part 1: Identify which virtual methods get invoked via invoke-super --- we'll
// stay away from those virtual scopes
// TODO: Relax this. Some portions of those virtual scopes could still
// be handled
void VirtualMerging::find_unsupported_virtual_scopes() {
  ConcurrentSet<const DexMethod*> invoke_super_methods;
  ConcurrentSet<const DexMethodRef*> invoke_super_unresolved_method_refs;
  walk::parallel::opcodes(
      m_scope,
      [](const DexMethod*) { return true; },
      [&](const DexMethod*, IRInstruction* insn) {
        if (insn->opcode() == OPCODE_INVOKE_SUPER) {
          auto method_ref = insn->get_method();
          auto method = resolve_method(method_ref, MethodSearch::Virtual);
          if (method == nullptr) {
            invoke_super_unresolved_method_refs.insert(method_ref);
          } else {
            invoke_super_methods.insert(method);
          }
        }
      });

  m_stats.invoke_super_methods = invoke_super_methods.size();
  m_stats.invoke_super_unresolved_method_refs =
      invoke_super_unresolved_method_refs.size();

  for (auto method : invoke_super_methods) {
    m_unsupported_virtual_scopes.insert(
        m_type_system.find_virtual_scope(method));
  }

  for (auto method : invoke_super_unresolved_method_refs) {
    m_unsupported_named_protos[method->get_name()].insert(method->get_proto());
  }
}

// Part 2: Identify all overriding virtual methods which might potentially be
//         mergeable into other overridden virtual methods.
//         Group these methods by virtual scopes.
void VirtualMerging::compute_mergeable_scope_methods() {
  walk::parallel::methods(m_scope, [&](const DexMethod* overriding_method) {
    if (!overriding_method->is_virtual() || !overriding_method->is_concrete() ||
        is_native(overriding_method) || is_abstract(overriding_method)) {
      return;
    }
    always_assert(overriding_method->is_def());
    always_assert(overriding_method->is_concrete());
    always_assert(!overriding_method->is_external());
    always_assert(overriding_method->get_code());

    auto virtual_scope = m_type_system.find_virtual_scope(overriding_method);
    if (virtual_scope == nullptr) {
      TRACE(VM, 1, "[VM] virtual method {%s} has no virtual scope!",
            SHOW(overriding_method));
      return;
    }
    if (virtual_scope->type == overriding_method->get_class()) {
      // Actually, this method isn't overriding anything.
      return;
    }

    if (m_unsupported_virtual_scopes.count(virtual_scope)) {
      TRACE(VM, 5, "[VM] virtual method {%s} in an unsupported virtual scope",
            SHOW(overriding_method));
      return;
    }

    auto it = m_unsupported_named_protos.find(overriding_method->get_name());
    if (it != m_unsupported_named_protos.end() &&
        it->second.count(overriding_method->get_proto())) {
      // Never observed in practice, but I guess it might happen
      TRACE(VM, 1, "[VM] virtual method {%s} has unsupported name/proto",
            SHOW(overriding_method));
      return;
    }

    m_mergeable_scope_methods.update(
        virtual_scope,
        [&](const VirtualScope*,
            std::unordered_set<const DexMethod*>& s,
            bool /* exists */) { s.emplace(overriding_method); });
  });

  m_stats.mergeable_scope_methods = m_mergeable_scope_methods.size();
  for (auto& p : m_mergeable_scope_methods) {
    m_stats.mergeable_virtual_methods += p.second.size();
  }
}

namespace {

struct LocalStats {
  size_t overriding_methods{0};
  size_t cross_store_refs{0};
  size_t cross_dex_refs{0};
  size_t inconcrete_overridden_methods{0};
};

class MergePairsBuilder {
  using MergablesMap = std::unordered_map<const DexMethod*, const DexMethod*>;

 public:
  using PairSeq = std::vector<std::pair<const DexMethod*, const DexMethod*>>;

  explicit MergePairsBuilder(const VirtualScope* virtual_scope)
      : virtual_scope(virtual_scope) {}

  boost::optional<std::pair<LocalStats, PairSeq>> build(
      const std::unordered_set<const DexMethod*>& mergeable_methods,
      const XStoreRefs& xstores,
      const XDexRefs& xdexes,
      const method_profiles::MethodProfiles& profiles) {
    if (!init()) {
      return boost::none;
    }

    MergablesMap mergeable_pairs_map =
        find_overrides(mergeable_methods, xstores, xdexes);

    if (mergeable_pairs_map.empty()) {
      always_assert(stats.overriding_methods ==
                    stats.cross_store_refs + stats.cross_dex_refs +
                        stats.inconcrete_overridden_methods);
      return std::make_pair(stats, PairSeq{});
    }

    auto mergeable_pairs =
        create_merge_pair_sequence(mergeable_pairs_map, profiles);
    return std::make_pair(stats, mergeable_pairs);
  }

 private:
  bool init() {
    for (auto& p : virtual_scope->methods) {
      auto method = p.first;
      methods.push_back(method);
      types_to_methods.emplace(method->get_class(), method);
      if (!can_rename(method) || root(method) ||
          method->rstate.no_optimizations()) {
        // If we find any method in this virtual scope which we shouldn't
        // touch, we exclude the entire virtual scope.
        return false;
      }
    }
    return true;
  }

  MergablesMap find_overrides(
      const std::unordered_set<const DexMethod*>& mergeable_methods,
      const XStoreRefs& xstores,
      const XDexRefs& xdexes) {
    MergablesMap mergeable_pairs_map;
    // sorting to make things deterministic
    std::sort(methods.begin(), methods.end(), dexmethods_comparator());
    for (DexMethod* overriding_method : methods) {
      if (!mergeable_methods.count(overriding_method)) {
        continue;
      }
      stats.overriding_methods++;
      auto subtype = overriding_method->get_class();
      always_assert(subtype != virtual_scope->type);
      auto overriding_cls = type_class(overriding_method->get_class());
      always_assert(overriding_cls != nullptr);
      auto supertype = overriding_cls->get_super_class();
      always_assert(supertype != nullptr);

      auto run_fn = [](auto fn, DexType* start, DexType* trailing,
                       const DexType* stop) {
        for (;;) {
          if (fn(start, trailing)) {
            return true;
          }
          if (start == stop) {
            return false;
          }
          trailing = start;
          start = type_class(start)->get_super_class();
        }
      };

      run_fn(
          [this](const DexType* t, DexType* trailing) {
            subtypes[t].push_back(trailing);
            return false;
          },
          supertype, subtype, virtual_scope->type);

      bool found_override = run_fn(
          [this, &overriding_method, &mergeable_pairs_map, &xstores,
           &xdexes](const DexType* t, const DexType*) {
            auto it = types_to_methods.find(t);
            if (it == types_to_methods.end()) {
              return false;
            }
            auto overridden_method = it->second;
            if (!overridden_method->is_concrete() ||
                is_native(overridden_method)) {
              stats.inconcrete_overridden_methods++;
            } else if (xstores.cross_store_ref(overridden_method,
                                               overriding_method)) {
              stats.cross_store_refs++;
            } else if (xdexes.cross_dex_ref_override(overridden_method,
                                                     overriding_method) ||
                       (xdexes.num_dexes() > 1 &&
                        xdexes.is_in_primary_dex(overridden_method))) {
              stats.cross_dex_refs++;
            } else {
              always_assert(overriding_method->get_code());
              always_assert(is_abstract(overridden_method) ||
                            overridden_method->get_code());
              mergeable_pairs_map.emplace(overriding_method, overridden_method);
            }
            return true;
          },
          supertype, subtype, virtual_scope->type);
      always_assert(found_override);
    }

    return mergeable_pairs_map;
  }

  PairSeq create_merge_pair_sequence(
      const MergablesMap& mergeable_pairs_map,
      const method_profiles::MethodProfiles& profiles) {
    // we do a depth-first traversal of the subtype structure, adding
    // mergeable pairs as we find them; this ensures that mergeable pairs
    // can later be processed sequentially --- first inlining pairs that
    // appear in deeper portions of the type hierarchy
    PairSeq mergeable_pairs;
    std::unordered_set<const DexType*> visited;
    std::unordered_map<const DexMethod*,
                       std::vector<std::pair<const DexMethod*, double>>>
        override_map;
    self_recursive_fn(
        [&](auto self, const DexType* t) {
          if (visited.count(t)) {
            return;
          }
          visited.insert(t);

          auto subtypes_it = subtypes.find(t);
          if (subtypes_it != subtypes.end()) {
            // This is ordered because `methods` was ordered.
            for (auto subtype : subtypes_it->second) {
              self(self, subtype);
            }
          }

          const DexMethod* t_method;
          {
            auto t_method_it = types_to_methods.find(t);
            if (t_method_it == types_to_methods.end()) {
              return;
            }
            t_method = t_method_it->second;
          }
          double acc_call_count = 0;
          if (auto mstats = profiles.get_method_stat(
                  method_profiles::COLD_START, t_method)) {
            acc_call_count = mstats->call_count;
          }

          {
            // If there are overrides for this type's implementation, order the
            // overrides by their weight (and otherwise retain the original
            // order), then insert the overrides into the global merge
            // structure.
            auto it = override_map.find(t_method);
            if (it != override_map.end()) {
              auto& t_overrides = it->second;
              redex_assert(!t_overrides.empty());
              // Use stable sort to retain order if call count is unavailable.
              // As insertion is pushing to front, sort low to high.
              std::stable_sort(t_overrides.begin(), t_overrides.end(),
                               [](const auto& lhs, const auto& rhs) {
                                 return lhs.second < rhs.second;
                               });
              for (const auto& p : t_overrides) {
                auto assert_it = mergeable_pairs_map.find(p.first);
                redex_assert(assert_it != mergeable_pairs_map.end() &&
                             assert_it->second == t_method);
                mergeable_pairs.emplace_back(t_method, p.first);
                acc_call_count += p.second;
              }
              // Clear the vector. Leave it empty for the assert above
              // (to ensure things are not handled twice).
              t_overrides.clear();
              t_overrides.shrink_to_fit();
            }
          }

          auto overridden_method_it = mergeable_pairs_map.find(t_method);
          if (overridden_method_it == mergeable_pairs_map.end()) {
            return;
          }
          override_map[overridden_method_it->second].emplace_back(
              t_method, acc_call_count);
        },
        virtual_scope->type);
    for (const auto& p : override_map) {
      redex_assert(p.second.empty());
    }
    always_assert(mergeable_pairs_map.size() == mergeable_pairs.size());
    always_assert(stats.overriding_methods ==
                  mergeable_pairs.size() + stats.cross_store_refs +
                      stats.cross_dex_refs +
                      stats.inconcrete_overridden_methods);
    return mergeable_pairs;
  }

  const VirtualScope* virtual_scope;
  std::vector<DexMethod*> methods;
  std::unordered_map<const DexType*, DexMethod*> types_to_methods;
  std::unordered_map<const DexType*, std::vector<DexType*>> subtypes;
  LocalStats stats;
};

} // namespace

// Part 3: For each virtual scope, identify all pairs of methods where
//         one can be merged with another. The list of pairs is ordered in
//         way that it can be later processed sequentially.
void VirtualMerging::compute_mergeable_pairs_by_virtual_scopes(
    const method_profiles::MethodProfiles& profiles) {
  ConcurrentMap<const VirtualScope*, LocalStats> local_stats;
  std::vector<const VirtualScope*> virtual_scopes;
  for (auto& p : m_mergeable_scope_methods) {
    virtual_scopes.push_back(p.first);
  }
  ConcurrentMap<const VirtualScope*,
                std::vector<std::pair<const DexMethod*, const DexMethod*>>>
      mergeable_pairs_by_virtual_scopes;
  walk::parallel::virtual_scopes(
      virtual_scopes, [&](const VirtualScope* virtual_scope) {
        MergePairsBuilder mpb(virtual_scope);
        auto res = mpb.build(m_mergeable_scope_methods.at(virtual_scope),
                             m_xstores, m_xdexes, profiles);
        if (!res) {
          return;
        }
        local_stats.emplace(virtual_scope, res->first);
        if (!res->second.empty()) {
          mergeable_pairs_by_virtual_scopes.emplace(virtual_scope,
                                                    std::move(res->second));
        }
      });

  m_stats.virtual_scopes_with_mergeable_pairs +=
      mergeable_pairs_by_virtual_scopes.size();

  size_t overriding_methods = 0;
  for (auto& p : local_stats) {
    overriding_methods += p.second.overriding_methods;
    m_stats.cross_store_refs += p.second.cross_store_refs;
    m_stats.cross_dex_refs += p.second.cross_dex_refs;
    m_stats.inconcrete_overridden_methods +=
        p.second.inconcrete_overridden_methods;
  }

  always_assert(overriding_methods <= m_stats.mergeable_virtual_methods);
  m_stats.annotated_methods =
      m_stats.mergeable_virtual_methods - overriding_methods;
  for (auto& p : mergeable_pairs_by_virtual_scopes) {
    const auto& mergeable_pairs = p.second;
    m_stats.mergeable_pairs += mergeable_pairs.size();
    m_mergeable_pairs_by_virtual_scopes.insert(p);
  }
  always_assert(mergeable_pairs_by_virtual_scopes.size() ==
                m_mergeable_pairs_by_virtual_scopes.size());
  always_assert(m_stats.mergeable_pairs ==
                m_stats.mergeable_virtual_methods - m_stats.annotated_methods -
                    m_stats.cross_store_refs - m_stats.cross_dex_refs -
                    m_stats.inconcrete_overridden_methods);
}

// Part 4: For each virtual scope, merge all pairs in order, unless inlining
//         is for some reason not possible, e.g. because of code size
//         constraints. Record set of methods in each class which can be
//         removed.
void VirtualMerging::merge_methods() {
  for (auto& p : m_mergeable_pairs_by_virtual_scopes) {
    auto virtual_scope = p.first;
    const auto& mergeable_pairs = p.second;
    for (auto& q : mergeable_pairs) {
      auto overridden_method = const_cast<DexMethod*>(q.first);
      auto overriding_method = const_cast<DexMethod*>(q.second);

      if (overriding_method->get_code()->sum_opcode_sizes() >
          m_max_overriding_method_instructions) {
        TRACE(VM,
              5,
              "[VM] %s is too large to be merged into %s",
              SHOW(overriding_method),
              SHOW(overridden_method));
        m_stats.huge_methods++;
        continue;
      }
      size_t estimated_insn_size =
          is_abstract(overridden_method)
              ? 64 // we'll need some extra instruction; 64 is conservative
              : overridden_method->get_code()->sum_opcode_sizes();
      std::vector<DexMethod*> make_static;
      if (!m_inliner->is_inlinable(overridden_method, overriding_method,
                                   nullptr /* invoke_virtual_insn */,
                                   estimated_insn_size, &make_static)) {
        TRACE(VM,
              3,
              "[VM] Cannot inline %s into %s",
              SHOW(overriding_method),
              SHOW(overridden_method));
        m_stats.uninlinable_methods++;
        continue;
      }
      m_inliner->make_static_inlinable(make_static);
      TRACE(VM,
            4,
            "[VM] Merging %s into %s",
            SHOW(overriding_method),
            SHOW(overridden_method));

      auto proto = overriding_method->get_proto();
      always_assert(overridden_method->get_proto() == proto);
      std::vector<uint32_t> param_regs;
      std::function<void(IRInstruction*)> push_insn;
      std::function<uint32_t()> allocate_temp;
      std::function<uint32_t()> allocate_wide_temp;
      std::function<void()> cleanup;
      IRCode* overridden_code;
      // We make the method public to avoid visibility issues. We could be more
      // conservative (i.e. taking the strongest visibility control that
      // encompasses the original pair) but I'm not sure it's worth the effort.
      set_public(overridden_method);
      if (is_abstract(overridden_method)) {
        // We'll make the abstract method be not abstract, and give it a new
        // method body.
        // It starts out with just load-param instructions as needed, and then
        // we'll add an invoke-virtual instruction that will get inlined.
        m_stats.unabstracted_methods++;
        overridden_method->make_concrete(
            (DexAccessFlags)(overridden_method->get_access() & ~ACC_ABSTRACT),
            std::make_unique<IRCode>(),
            true /* is_virtual */);
        overridden_code = overridden_method->get_code();
        auto load_param_insn = new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
        load_param_insn->set_dest(overridden_code->allocate_temp());
        overridden_code->push_back(load_param_insn);
        param_regs.push_back(load_param_insn->dest());
        for (auto t : proto->get_args()->get_type_list()) {
          if (type::is_wide_type(t)) {
            load_param_insn = new IRInstruction(IOPCODE_LOAD_PARAM_WIDE);
            load_param_insn->set_dest(overridden_code->allocate_wide_temp());
          } else {
            load_param_insn =
                new IRInstruction(type::is_object(t) ? IOPCODE_LOAD_PARAM_OBJECT
                                                     : IOPCODE_LOAD_PARAM);
            load_param_insn->set_dest(overridden_code->allocate_temp());
          }
          overridden_code->push_back(load_param_insn);
          param_regs.push_back(load_param_insn->dest());
        }
        // we'll define helper functions in a way that lets them mutate the new
        // IRCode
        push_insn = [=](IRInstruction* insn) {
          overridden_code->push_back(insn);
        };
        allocate_temp = [=]() { return overridden_code->allocate_temp(); };
        allocate_wide_temp = [=]() {
          return overridden_code->allocate_wide_temp();
        };
        cleanup = [=]() { overridden_code->build_cfg(/* editable */ true); };
      } else {
        // We are dealing with a non-abstract method. In this case, we'll first
        // insert an if-instruction to decide whether to run the overriding
        // method that we'll inline, or whether to jump to the old method body.
        overridden_code = overridden_method->get_code();
        always_assert(overridden_code);
        overridden_code->build_cfg(/* editable */ true);
        auto& overridden_cfg = overridden_code->cfg();

        // Find block with load-param instructions
        cfg::Block* block = overridden_cfg.entry_block();
        while (block->get_first_insn() == block->end()) {
          const auto& succs = block->succs();
          always_assert(succs.size() == 1);
          const auto& out = succs[0];
          always_assert(out->type() == cfg::EDGE_GOTO);
          block = out->target();
        }

        // Scan load-param instructions
        std::unordered_set<uint32_t> param_regs_set;
        auto last_it = block->end();
        for (auto it = block->begin(); it != block->end(); it++) {
          auto& mie = *it;
          if (!opcode::is_a_load_param(mie.insn->opcode())) {
            break;
          }
          param_regs.push_back(mie.insn->dest());
          param_regs_set.insert(mie.insn->dest());
          last_it = it;
        }
        always_assert(param_regs.size() == param_regs_set.size());
        always_assert(1 + proto->get_args()->get_type_list().size() ==
                      param_regs_set.size());
        always_assert(last_it != block->end());

        // We'll split the block right after the last load-param instruction ---
        // that's where we'll insert the new if-statement.
        overridden_cfg.split_block(block->to_cfg_instruction_iterator(last_it));
        auto new_block = overridden_cfg.create_block();
        {
          // instance-of param0, DeclaringTypeOfOverridingMethod
          auto instance_of_insn = new IRInstruction(OPCODE_INSTANCE_OF);
          instance_of_insn->set_type(overriding_method->get_class());
          instance_of_insn->set_src(0, param_regs.at(0));
          block->push_back(instance_of_insn);
          // move-result-pseudo if_temp
          auto if_temp_reg = overridden_cfg.allocate_temp();
          auto move_result_pseudo_insn =
              new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO);
          move_result_pseudo_insn->set_dest(if_temp_reg);
          block->push_back(move_result_pseudo_insn);
          auto if_insn = new IRInstruction(OPCODE_IF_NEZ);
          if_insn->set_src(0, if_temp_reg);
          // if-nez if_temp, new_code
          // (fall through to old code)
          overridden_cfg.create_branch(
              block, if_insn, block->goes_to() /* false */, new_block
              /* true */);
        }
        // we'll define helper functions in a way that lets them mutate the cfg
        push_insn = [=](IRInstruction* insn) { new_block->push_back(insn); };
        auto* cfg_ptr = &overridden_cfg;
        allocate_temp = [=]() { return cfg_ptr->allocate_temp(); };
        allocate_wide_temp = [=]() { return cfg_ptr->allocate_wide_temp(); };
        cleanup = []() {};
      }
      always_assert(1 + proto->get_args()->get_type_list().size() ==
                    param_regs.size());

      // invoke-virtual temp, param1, ..., paramN, OverridingMethod
      auto invoke_virtual_insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
      invoke_virtual_insn->set_method(overriding_method);
      invoke_virtual_insn->set_srcs_size(param_regs.size());
      for (size_t i = 0; i < param_regs.size(); i++) {
        uint32_t reg = param_regs[i];
        if (i == 0) {
          uint32_t temp_reg = allocate_temp();
          auto check_cast_insn = new IRInstruction(OPCODE_CHECK_CAST);
          check_cast_insn->set_type(overriding_method->get_class());
          check_cast_insn->set_src(0, reg);
          push_insn(check_cast_insn);
          auto move_result_pseudo_insn =
              new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
          move_result_pseudo_insn->set_dest(temp_reg);
          push_insn(move_result_pseudo_insn);
          reg = temp_reg;
        }
        invoke_virtual_insn->set_src(i, reg);
      }
      push_insn(invoke_virtual_insn);
      if (proto->is_void()) {
        // return-void
        auto return_insn = new IRInstruction(OPCODE_RETURN_VOID);
        push_insn(return_insn);
      } else {
        // move-result result_temp
        auto rtype = proto->get_rtype();
        auto op = opcode::move_result_for_invoke(overriding_method);
        auto move_result_insn = new IRInstruction(op);
        auto result_temp = op == OPCODE_MOVE_RESULT_WIDE ? allocate_wide_temp()
                                                         : allocate_temp();
        move_result_insn->set_dest(result_temp);
        push_insn(move_result_insn);
        // return result_temp
        op = opcode::return_opcode(rtype);
        auto return_insn = new IRInstruction(op);
        return_insn->set_src(0, result_temp);
        push_insn(return_insn);
      }

      cleanup();

      overriding_method->get_code()->build_cfg(/* editable */ true);
      inliner::inline_with_cfg(
          overridden_method, overriding_method, invoke_virtual_insn,
          overridden_method->get_code()->cfg().get_registers_size());
      change_visibility(overriding_method, overridden_method->get_class());
      overriding_method->get_code()->clear_cfg();

      // Check if everything was inlined.
      for (const auto& mie : cfg::InstructionIterable(overridden_code->cfg())) {
        redex_assert(invoke_virtual_insn != mie.insn);
      }

      overridden_code->clear_cfg();

      m_virtual_methods_to_remove[type_class(overriding_method->get_class())]
          .push_back(overriding_method);
      auto virtual_scope_root = virtual_scope->methods.front();
      always_assert(overriding_method != virtual_scope_root.first);
      m_virtual_methods_to_remap.emplace(overriding_method,
                                         virtual_scope_root.first);
      m_stats.removed_virtual_methods++;
    }
  }

  always_assert(m_stats.mergeable_pairs == m_stats.huge_methods +
                                               m_stats.uninlinable_methods +
                                               m_stats.removed_virtual_methods);
}

// Part 5: Remove methods within classes.
void VirtualMerging::remove_methods() {
  std::vector<DexClass*> classes_with_virtual_methods_to_remove;
  for (auto& p : m_virtual_methods_to_remove) {
    classes_with_virtual_methods_to_remove.push_back(p.first);
  }

  walk::parallel::classes(
      classes_with_virtual_methods_to_remove, [&](DexClass* cls) {
        for (auto method : m_virtual_methods_to_remove.at(cls)) {
          cls->remove_method(method);
        }
      });
}

// Part 6: Remap all invoke-virtual instructions where the associated method got
// removed
void VirtualMerging::remap_invoke_virtuals() {
  walk::parallel::opcodes(
      m_scope,
      [](const DexMethod*) { return true; },
      [&](const DexMethod*, IRInstruction* insn) {
        if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
          auto method_ref = insn->get_method();
          auto method = resolve_method(method_ref, MethodSearch::Virtual);
          auto it = m_virtual_methods_to_remap.find(method);
          if (it != m_virtual_methods_to_remap.end()) {
            insn->set_method(it->second);
          }
        }
      });
}

void VirtualMerging::run(const method_profiles::MethodProfiles& profiles) {
  TRACE(VM, 1, "[VM] Finding unsupported virtual scopes");
  find_unsupported_virtual_scopes();
  TRACE(VM, 1, "[VM] Computing mergeable scope methods");
  compute_mergeable_scope_methods();
  TRACE(VM, 1, "[VM] Computing mergeable pairs by virtual scopes");
  compute_mergeable_pairs_by_virtual_scopes(profiles);
  TRACE(VM, 1, "[VM] Merging methods");
  merge_methods();
  TRACE(VM, 1, "[VM] Removing methods");
  remove_methods();
  TRACE(VM, 1, "[VM] Remapping invoke-virtual instructions");
  remap_invoke_virtuals();
  TRACE(VM, 1, "[VM] Done");
}

void VirtualMergingPass::bind_config() {
  // Merging huge overriding methods into an overridden method tends to not
  // be a good idea, as it may pull in many other dependencies, and all just
  // for some small saving in number of method refs. So we impose a configurable
  // limit.
  int64_t default_max_overriding_method_instructions = 1000;
  bind("max_overriding_method_instructions",
       default_max_overriding_method_instructions,
       m_max_overriding_method_instructions);
  bind("use_profiles", true, m_use_profiles);

  after_configuration(
      [this] { always_assert(m_max_overriding_method_instructions >= 0); });
}

void VirtualMergingPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  if (mgr.get_redex_options().instrument_pass_enabled) {
    TRACE(VM,
          1,
          "Skipping VirtualMergingPass because Instrumentation is enabled");
    return;
  }

  auto dedupped = dedup_vmethods::dedup(stores);

  const auto& inliner_config = conf.get_inliner_config();
  VirtualMerging vm(stores, inliner_config,
                    m_max_overriding_method_instructions);
  vm.run(m_use_profiles ? conf.get_method_profiles()
                        : method_profiles::MethodProfiles());
  auto stats = vm.get_stats();

  mgr.incr_metric(METRIC_DEDUPPED_VIRTUAL_METHODS, dedupped);
  mgr.incr_metric(METRIC_INVOKE_SUPER_METHODS, stats.invoke_super_methods);
  mgr.incr_metric(METRIC_INVOKE_SUPER_UNRESOLVED_METHOD_REFS,
                  stats.invoke_super_unresolved_method_refs);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS,
                  stats.mergeable_virtual_methods);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS_ANNOTATED_METHODS,
                  stats.annotated_methods);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_STORE_REFS,
                  stats.cross_store_refs);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_METHODS_CROSS_DEX_REFS,
                  stats.cross_dex_refs);
  mgr.incr_metric(
      METRIC_MERGEABLE_VIRTUAL_METHODS_INCONCRETE_OVERRIDDEN_METHODS,
      stats.inconcrete_overridden_methods);
  mgr.incr_metric(METRIC_MERGEABLE_VIRTUAL_SCOPES,
                  stats.mergeable_scope_methods);
  mgr.incr_metric(METRIC_MERGEABLE_PAIRS, stats.mergeable_pairs);
  mgr.incr_metric(METRIC_VIRTUAL_SCOPES_WITH_MERGEABLE_PAIRS,
                  stats.virtual_scopes_with_mergeable_pairs);
  mgr.incr_metric(METRIC_UNABSTRACTED_METHODS, stats.unabstracted_methods);
  mgr.incr_metric(METRIC_UNINLINABLE_METHODS, stats.uninlinable_methods);
  mgr.incr_metric(METRIC_HUGE_METHODS, stats.huge_methods);
  mgr.incr_metric(METRIC_REMOVED_VIRTUAL_METHODS,
                  stats.removed_virtual_methods);
}

static VirtualMergingPass s_pass;
