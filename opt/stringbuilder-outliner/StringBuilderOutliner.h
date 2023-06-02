/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional.hpp>

#include "AbstractDomain.h"
#include "LocalPointersAnalysis.h"
#include "Pass.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

/*
 * This pass looks for recurring sequences of StringBuilder calls and outlines
 * them. This outlining is special-cased because StringBuilders are one of the
 * most commonly instantiated objects in Java code, and because we can use
 * knowledge of the semantics of StringBuilder methods to perform code motion
 * as part of that outlining. In particular, StringBuilder calls tend to occur
 * in the following pattern:
 *
 *   new-instance v0 StringBuilder;
 *   invoke-direct v0 StringBuilder;.<init>:()V
 *   [sget v1 Foo;.bar:I | iget-object v1 v2 Foo;.baz:I | ...]
 *   invoke-virtual {v0, v1} StringBuilder;.append:(I)Ljava/lang/StringBuilder;
 *   [sget v1 Foo;.bar:I | iget-object v1 v2 Foo;.baz:I | ...]
 *   invoke-virtual {v0, v1} StringBuilder;.append:(I)Ljava/lang/StringBuilder;
 *   [sget v1 Foo;.bar:I | iget-object v1 v2 Foo;.baz:I | ...]
 *   invoke-virtual {v0, v1} StringBuilder;.append:(I)Ljava/lang/StringBuilder;
 *   invoke-virtual v0 StringBuilder;.toString:()Ljava/lang/String;
 *   move-result-object v0
 *
 * The instructions inside [...] denote a variety of possible instructions that
 * can generate the values passed to append(). Since these value-generating
 * instructions tend to vary between StringBuilder use sites, an outliner that
 * tries to factor out common patterns without reordering instructions would
 * be thwarted by them. However, since we know that StringBuilder methods are
 * independent of any state in user code, we can safely move them down to create
 * contiguous sequences of repetitive code:
 *
 *   [sget v1 Foo;.bar:I | iget-object v1 v4 Foo;.baz:I | ...]
 *   [sget v2 Foo;.bar:I | iget-object v2 v4 Foo;.baz:I | ...]
 *   [sget v3 Foo;.bar:I | iget-object v3 v4 Foo;.baz:I | ...]
 *   // Beginning of outlinable section
 *   new-instance v0 StringBuilder;
 *   invoke-direct v0 StringBuilder;.<init>:()V
 *   invoke-virtual {v0, v1} StringBuilder;.append:(I)Ljava/lang/StringBuilder;
 *   invoke-virtual {v0, v2} StringBuilder;.append:(I)Ljava/lang/StringBuilder;
 *   invoke-virtual {v0, v3} StringBuilder;.append:(I)Ljava/lang/StringBuilder;
 *   invoke-virtual v0 StringBuilder;.toString:()Ljava/lang/String;
 *   move-result-object v0
 *
 * This code reordering is conceptual -- we don't actually perform the
 * reordering separately from the outlining. Instead, we use Abstract
 * Interpretation to model the state of StringBuilder instances, so we can
 * generate outlined code based on that state.
 *
 * Note that this transformation means that the StringBuilder instance is no
 * longer accessible in the caller. That means that it cannot be used by any
 * operations aside from those in the outlined code. It is a little tricky to
 * do this analysis, so we defer it to a later run of the ObjectSensitiveDce
 * pass. Here we just replace calls to StringBuilder.toString() with calls to
 * the outline helper functions and assume that in most cases the StringBuilder
 * instance and the append operations on them are going to be removable by
 * OSDCE. This is generally true in practice.
 */

namespace stringbuilder_outliner {

/*
 * The sequence of StringBuilder method calls that have been invoked on a given
 * StringBuilder instance.
 */
using BuilderState = std::vector<const IRInstruction*>;

class BuilderValue final : public sparta::AbstractValue<BuilderValue> {
 public:
  BuilderValue() = default;

  void clear() { m_state.clear(); }

  sparta::AbstractValueKind kind() const {
    return sparta::AbstractValueKind::Value;
  }

  bool leq(const BuilderValue& other) const { return equals(other); }

  bool equals(const BuilderValue& other) const {
    return m_state == other.m_state;
  }

  sparta::AbstractValueKind join_with(const BuilderValue& other) {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }
    return sparta::AbstractValueKind::Top;
  }

  sparta::AbstractValueKind widen_with(const BuilderValue& other) {
    return join_with(other);
  }

  sparta::AbstractValueKind meet_with(const BuilderValue& other) {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }
    return sparta::AbstractValueKind::Bottom;
  }

  sparta::AbstractValueKind narrow_with(const BuilderValue& other) {
    return meet_with(other);
  }

  const BuilderState& state() const { return m_state; }

  void add_operation(const IRInstruction* insn) { m_state.emplace_back(insn); }

 private:
  BuilderState m_state;
};

class BuilderDomain final
    : public sparta::AbstractDomainScaffolding<BuilderValue, BuilderDomain> {
 public:
  // Inherit constructors from AbstractDomainScaffolding.
  using AbstractDomainScaffolding::AbstractDomainScaffolding;

  // Constructor inheritance is buggy in some versions of gcc, hence the
  // redefinition of the default constructor.
  BuilderDomain() {}

  void add_operation(const IRInstruction* insn) {
    if (kind() == sparta::AbstractValueKind::Value) {
      get_value()->add_operation(insn);
    }
  }

  boost::optional<BuilderState> state() const {
    return (kind() == sparta::AbstractValueKind::Value)
               ? boost::optional<BuilderState>(get_value()->state())
               : boost::none;
  }
};

/*
 * A model of StringBuilder objects stored on the heap.
 */
class BuilderStore {
 public:
  using Domain =
      sparta::PatriciaTreeMapAbstractEnvironment<const IRInstruction*,
                                                 BuilderDomain>;

  static void set_may_escape(const IRInstruction* ptr,
                             const IRInstruction* /* blame */,
                             Domain* dom) {
    dom->set(ptr, BuilderDomain::top());
  }

  static void set_fresh(const IRInstruction* ptr, Domain* dom) {
    dom->set(ptr, BuilderDomain());
  }

  static bool may_have_escaped(const Domain& dom, const IRInstruction* ptr) {
    return dom.get(ptr).is_top();
  }
};

using Environment = local_pointers::EnvironmentWithStoreImpl<BuilderStore>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg);

  void analyze_instruction(const IRInstruction* insn,
                           Environment* env) const override;

 private:
  bool is_eligible_init(const DexMethodRef*) const;
  bool is_eligible_append(const DexMethodRef*) const;

  const DexType* m_stringbuilder;
  std::unordered_set<DexType*> m_immutable_types;
  const DexMethodRef* m_stringbuilder_no_param_init;
  const DexMethodRef* m_stringbuilder_init_with_string;
  const DexString* m_append_str;
};

using InstructionSet = std::unordered_set<const IRInstruction*>;

using BuilderStateMap =
    std::vector<std::pair<const IRInstruction*, BuilderState>>;

struct Config {
  size_t max_outline_length{9};
  size_t min_outline_count{10};
};

struct Stats {
  size_t stringbuilders_removed{0};
  size_t operations_removed{0};
  size_t helper_methods_created{0};
};

class Outliner {
 public:
  explicit Outliner(Config config = Config());

  const Config& get_config() const { return m_config; }

  const Stats& get_stats() const { return m_stats; }

  void analyze(IRCode& code);

  void create_outline_helpers(DexStoresVector* stores);

  void transform(IRCode* code);

 private:
  InstructionSet find_tostring_instructions(
      const cfg::ControlFlowGraph& cfg) const;

  BuilderStateMap gather_builder_states(
      const cfg::ControlFlowGraph& cfg,
      const InstructionSet& eligible_tostring_instructions) const;

  const DexTypeList* typelist_from_state(const BuilderState& state) const;

  void gather_outline_candidate_typelists(
      const BuilderStateMap& tostring_instruction_to_state);

  std::unique_ptr<IRCode> create_outline_helper_code(DexMethod*) const;

  static void apply_changes(
      const std::unordered_map<const IRInstruction*, IRInstruction*>&
          insns_to_insert,
      const std::unordered_map<const IRInstruction*, IRInstruction*>&
          insns_to_replace,
      IRCode* code);

  Config m_config;
  Stats m_stats;

  const DexString* m_append_str;
  DexType* m_stringbuilder;
  DexMethodRef* m_stringbuilder_default_ctor;
  DexMethodRef* m_stringbuilder_capacity_ctor;
  DexMethodRef* m_stringbuilder_tostring;

  // Map typelists of potentially outlinable StringBuilder call sequence to
  // their number of occurrences.
  ConcurrentMap<const DexTypeList*, size_t> m_outline_typelists;
  // Typelists of call sequences we have chosen to outline -> generated outline
  // helper method.
  std::unordered_map<const DexTypeList*, DexMethod*> m_outline_helpers;

  ConcurrentMap<const IRCode*, BuilderStateMap> m_builder_state_maps;
};

class StringBuilderOutlinerPass : public Pass {
 public:
  StringBuilderOutlinerPass() : Pass("StringBuilderOutlinerPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  void bind_config() override {
    bind("max_outline_length", m_config.max_outline_length,
         m_config.max_outline_length);
    bind("min_outline_count", m_config.min_outline_count,
         m_config.min_outline_count);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  Config m_config;
};

} // namespace stringbuilder_outliner
