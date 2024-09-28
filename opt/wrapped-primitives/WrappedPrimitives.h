/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>
#include <variant>

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "Pass.h"
#include "Trace.h"

namespace wrapped_primitives {
// A config driven spec describing wrapper classes to look for, each of which is
// asserted to have 1 final field of some primitive type. Beyond this,
// assumptions also include:
// 1) A constructor taking 1 argument which is the primitive it wraps.
// 2) Wrapper class extends java.lang.Object and does not implement interfaces.
//
// Wrapper class instances that can effectively be "unboxed" by this pass must
// conform to a very narrow set of usages. Currently, supported uses are:
// - Wrapper class can be instantiated with a known constant (known means
//   intraprocedural constant propagation can easily figure it out).
// - Wrapper class instances can be written to static final fields.
// - Wrapper class instances can be retrieved from static final fields.
// - Wrapper class instances can be an argument to a set of configured "allowed
// - invokes" i.e. method refs that they can be passed to.
//
// Finally, the input program must honor guarantees about the allowed method
// invocations. For the output program to type check properly, it must be
// explicitly listed for every allowed API taking the wrapper class, what is the
// corresponding primitive API that should be swapped in. It is up to the author
// of the input program to ensure that this works in practice, otherwise Redex
// is free to fail in whatever way it chooses (i.e. fail the build or optimize
// no wrapper types).
//
// EXAMPLE:
// "LFoo;.a:(LMyLong;)V" is an allowed invoke, the config should map this to
// something like "LFoo;.a:(J)V" which will also need to exist in the input
// program. This is the simplest form. If however, the allowed invoke maps to an
// API on a different type, say from an interface method to a method on the
// interface's underlying implenentor, check-cast instructions may need to be
// inserted to make this work. It's up to the program's authors to ensure this
// ends up as a working app (and we may fail the build otherwise, or insert
// casts that would fail at runtime if things are misconfigured).
struct Spec {
  DexType* wrapper{nullptr};
  DexType* primitive{nullptr};
  std::map<DexMethodRef*, DexMethodRef*, dexmethods_comparator> allowed_invokes;

  std::vector<DexMethod*> wrapper_type_constructors() {
    auto cls = type_class(wrapper);
    return cls->get_ctors();
  }
};

// Details pertaining to an understood instantiation of a wrapper class with a
// known primitive given to its constructor.
struct Source {
  IRInstruction* new_instance;
  IRInstruction* init;
  DexMethod* method;
  int64_t primitive_value;
};

// A point in the code which a wrapper class is being used (beyond it being
// instantiation).
struct Usage {
  IRInstruction* insn;
  DexMethod* method;
};

// Represents a tree of instantiation (Source) to many Usages (which can have)
// their own uses. Equality checks are here to let this be built up in rounds.
struct Node {
  std::variant<Source, Usage> item;
  std::vector<std::unique_ptr<Node>> edges;
  std::unordered_set<IRInstruction*> seen;

  bool is_source() { return std::holds_alternative<Source>(item); }
  bool is_usage() { return std::holds_alternative<Usage>(item); }

  // For use-def analysis, the instruction that could be followed up by uses
  IRInstruction* get_def_instruction() {
    if (is_source()) {
      return std::get<Source>(item).new_instance;
    } else {
      return std::get<Usage>(item).insn;
    }
  }

  DexMethod* get_method() {
    if (is_source()) {
      return std::get<Source>(item).method;
    } else {
      return std::get<Usage>(item).method;
    }
  }

  void add_edge(std::unique_ptr<Node> node) {
    always_assert(node->is_usage());
    auto insn = std::get<Usage>(node->item).insn;
    if (seen.count(insn) == 0) {
      seen.emplace(insn);
      edges.emplace_back(std::move(node));
    }
  }
};

// Allow for Nodes to be built up sequentially in rounds, keeping track of only
// newly seen things.
struct Forest {
  std::vector<std::unique_ptr<Node>> nodes;
  std::unordered_set<IRInstruction*> seen;

  void add_node(std::unique_ptr<Node> node) {
    always_assert(node->is_source());
    auto insn = std::get<Source>(node->item).new_instance;
    if (seen.count(insn) == 0) {
      seen.emplace(insn);
      nodes.emplace_back(std::move(node));
    }
  }
};

// Global state of the pass as it analyzes static fields and their usages.
struct PassState {
  Forest forest;
  std::unordered_map<DexField*, Node*> sfield_to_node;
  constant_propagation::WholeProgramState whole_program_state;
  constant_propagation::ImmutableAttributeAnalyzerState attr_analyzer_state;
  // For modifications to the tree of source/usages.
  std::mutex modifications_mtx;
};

class MethodAnalysis {
 public:
  MethodAnalysis(const std::unordered_map<DexType*, Spec>& wrapper_types,
                 PassState* pass_state,
                 DexClass* cls,
                 DexMethod* method)
      : m_wrapper_types(wrapper_types),
        m_pass_state(pass_state),
        m_cls(cls),
        m_method(method),
        m_live_ranges([&]() {
          return std::make_unique<live_range::LazyLiveRanges>(get_cfg());
        }) {
    auto& cfg = get_cfg();
    cfg.calculate_exit_block();
  }

  virtual ~MethodAnalysis() {}

  cfg::ControlFlowGraph& get_cfg() { return m_method->get_code()->cfg(); }

  // Checks if the value is a known ObjectWithImmutAttr with a single known
  // attribute value. Makes assumptions that there is only 1, as is consistent
  // with the other assumptions in the pass.
  boost::optional<int64_t> extract_object_attr_value(
      const ConstantValue& value) {
    auto obj_or_none = value.maybe_get<ObjectWithImmutAttrDomain>();
    if (obj_or_none != boost::none &&
        obj_or_none->get_constant() != boost::none) {
      auto object = *obj_or_none->get_constant();
      always_assert(object.attributes.size() == 1);
      auto signed_value =
          object.attributes.front().value.maybe_get<SignedConstantDomain>();
      if (signed_value != boost::none &&
          signed_value.value().get_constant() != boost::none) {
        return *signed_value.value().get_constant();
      } else {
        TRACE(WP, 2, "  No SignedConstantDomain value");
      }
    } else {
      TRACE(WP, 2, "  Not a known ObjectWithImmutAttrDomain");
    }

    return boost::none;
  }

  // For a def instruction (asserted to be a new-instance), find the usage that
  // invokes the constructor. Asserts there is only 1.
  IRInstruction* find_invoke_ctor(IRInstruction* new_instance) {
    IRInstruction* invoke_ctor{nullptr};
    auto& uses = m_live_ranges->def_use_chains->at(new_instance);
    for (auto u : uses) {
      if (u.insn->opcode() == OPCODE_INVOKE_DIRECT &&
          method::is_init(u.insn->get_method())) {
        if (u.insn->get_method()->get_class() == new_instance->get_type()) {
          always_assert(invoke_ctor == nullptr);
          invoke_ctor = u.insn;
        }
      }
    }
    return invoke_ctor;
  }

  // For information about the instantiation or get of a wrapped type, attach
  // the node to the pass state's representation, along with nodes for all
  // immediate uses of the def.
  void attach_usage_nodes(
      std::unique_ptr<Node>& def_node,
      const std::unordered_set<IRInstruction*>& exceptions) {
    auto def = def_node->get_def_instruction();
    auto& uses = m_live_ranges->def_use_chains->at(def);
    TRACE(WP, 2, "%s has %zu use(s)", SHOW(def), uses.size());
    // Make nodes for the use(s)
    for (auto u : uses) {
      if (exceptions.count(u.insn) > 0) {
        continue;
      }
      Usage usage{u.insn, m_method};
      auto usage_node = std::make_unique<Node>();
      usage_node->item = usage;
      def_node->add_edge(std::move(usage_node));
    }
  }

  void attach_usage_nodes(std::unique_ptr<Node>& def_node) {
    attach_usage_nodes(def_node, {});
  }

  // Keeps track of global state for the node of a field, so that further usages
  // can be connected to the pass state's representation.
  void store_sput_node_pointer(std::unique_ptr<Node>& def_node,
                               DexField* put_field_def,
                               IRInstruction* sput) {
    for (auto& usage_node : def_node->edges) {
      auto usage = std::get<Usage>(usage_node->item);
      if (usage.insn == sput) {
        auto pair = m_pass_state->sfield_to_node.emplace(put_field_def,
                                                         usage_node.get());
        if (pair.second) {
          TRACE(WP,
                2,
                "  field %s will map to usage %p",
                SHOW(put_field_def),
                usage_node.get());
        } else {
          auto ptr = pair.first->second;
          TRACE(WP,
                2,
                "  field %s has redundant put; previous usage node %p will "
                "take effect",
                SHOW(put_field_def),
                ptr);
        }
      }
    }
  }

  // For a def that was instantiated by the method, emit a node and attach to
  // the pass state's representation.
  void emit_new_instance_node(const int64_t constant,
                              IRInstruction* new_instance,
                              DexField* put_field_def,
                              IRInstruction* sput) {
    auto invoke_ctor = find_invoke_ctor(new_instance);
    Source source{new_instance, invoke_ctor, m_method, constant};
    auto node = std::make_unique<Node>();
    node->item = source;
    // Find all users of the new-instance, add edges.
    attach_usage_nodes(node, {invoke_ctor});
    // Track sput-object specially, as explained above.
    store_sput_node_pointer(node, put_field_def, sput);
    // Connect this to the forest.
    m_pass_state->forest.add_node(std::move(node));
  }

  // For a def that was from an sget, emit a node and attach to the pass state's
  // representation.
  void emit_sget_node(IRInstruction* sget,
                      DexField* put_field_def,
                      IRInstruction* sput) {
    auto resolved_get_field_def =
        resolve_field(sget->get_field(), FieldSearch::Static);
    always_assert_log(resolved_get_field_def != nullptr,
                      "Unable to resolve field from instruction %s",
                      SHOW(sget));

    Usage sget_usage{sget, m_method};
    auto node = std::make_unique<Node>();
    node->item = sget_usage;

    // Find all users of the sget.
    attach_usage_nodes(node);
    // Track sput-object specially, as explained above.
    store_sput_node_pointer(node, put_field_def, sput);
    // Connect this to the appropriate parent.
    auto& parent = m_pass_state->sfield_to_node.at(resolved_get_field_def);
    parent->add_edge(std::move(node));
  }

  // Follow-up work after running the fixpoint iterator. Implementation specific
  virtual void post_analyze() {}

  void run(const InstructionAnalyzer<ConstantEnvironment>& insn_analyzer) {
    auto& cfg = get_cfg();
    TRACE(WP, 3, "Analyzing %s %s", SHOW(m_method), SHOW(cfg));
    m_fp_iter = std::make_unique<
        constant_propagation::intraprocedural::FixpointIterator>(
        /* cp_state */ nullptr, cfg, insn_analyzer);
    m_fp_iter->run(ConstantEnvironment());
    post_analyze();
  }

  constant_propagation::intraprocedural::FixpointIterator*
  get_fixpoint_iterator() {
    return m_fp_iter.get();
  }

 protected:
  const std::unordered_map<DexType*, Spec>& m_wrapper_types;
  PassState* m_pass_state;
  DexClass* m_cls;
  DexMethod* m_method;
  Lazy<live_range::LazyLiveRanges> m_live_ranges;

  std::unique_ptr<constant_propagation::intraprocedural::FixpointIterator>
      m_fp_iter;
};
} // namespace wrapped_primitives

// A wrapped primitive is a type with a constructor taking a primitive, that is
// largely used to achieve some special kind of type safety above just a
// primitive. Configurations will specify the wrapper type name, and APIs that
// it is sanctioned to be used in. For wrapper instances that can be replaced
// directly with the primitive itself safely (based on easily understood
// instantiation and no unsupported usages) this pass will make modifications.
class WrappedPrimitivesPass : public Pass {
 public:
  WrappedPrimitivesPass() : Pass("WrappedPrimitivesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {UltralightCodePatterns, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override;
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void unset_roots();

 private:
  std::vector<wrapped_primitives::Spec> m_wrapper_specs;
  // Config driven optimization will create inbound references to new methods.
  // These methods need to not be deleted.
  std::unordered_set<DexClass*> m_marked_root_classes;
  std::unordered_set<DexMethod*> m_marked_root_methods;
};
