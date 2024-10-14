/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationState.h"
#include "ConstantPropagationWholeProgramState.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "GlobalConfig.h"
#include "PassManager.h"
#include "Trace.h"
#include "TypeSystem.h"

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

struct KnownDef {
  const DexType* wrapper_type;
  IRInstruction* primary_insn;
  reg_t dest_reg;
  int64_t primitive_value;
};

namespace cp = constant_propagation;
class WrappedPrimitives {
 public:
  explicit WrappedPrimitives(const std::vector<Spec>& wrapper_specs)
      : m_wrapper_specs(wrapper_specs) {
    for (auto& spec : wrapper_specs) {
      TRACE(WP,
            1,
            "Will check for wrapper type %s with supported methods:",
            SHOW(spec.wrapper));
      auto wrapper_cls = type_class(spec.wrapper);
      always_assert(wrapper_cls != nullptr);
      m_type_to_spec.emplace(spec.wrapper, spec);
      for (auto&& [from, to] : spec.allowed_invokes) {
        TRACE(WP, 1, "  %s", SHOW(from));
      }
      for (auto&& [from, to] : spec.allowed_invokes) {
        m_all_wrapped_apis.emplace(from);
      }
    }
  }
  void mark_roots();
  void unmark_roots();
  std::unordered_map<IRInstruction*, KnownDef> build_known_definitions(
      const cp::intraprocedural::FixpointIterator& intra_cp,
      cfg::ControlFlowGraph& cfg);
  void optimize_method(const TypeSystem& type_system,
                       const cp::intraprocedural::FixpointIterator& intra_cp,
                       const cp::WholeProgramState& wps,
                       DexMethod* method,
                       cfg::ControlFlowGraph& cfg);
  // Stats
  size_t consts_inserted() { return m_consts_inserted; }
  size_t casts_inserted() { return m_casts_inserted; }
  // Convenience methods;
  bool is_wrapped_api(const DexMethodRef* ref) {
    return m_all_wrapped_apis.count(ref) > 0;
  }

 private:
  void increment_consts();
  void increment_casts();
  bool is_wrapped_method_ref(const DexType* wrapper_type, DexMethodRef* ref);
  // Checks what is configured for the given type and wrapped method ref;
  // asserts there is a primitive version of it.
  DexMethodRef* unwrapped_method_ref_for(const DexType* wrapper_type,
                                         DexMethodRef* ref);
  std::vector<Spec> m_wrapper_specs;
  std::unordered_map<DexType*, Spec> m_type_to_spec;
  std::unordered_set<const DexMethodRef*> m_all_wrapped_apis;
  // Config driven optimization will create inbound references to new methods.
  // These methods need to not be deleted.
  std::unordered_set<DexClass*> m_marked_root_classes;
  std::unordered_set<DexMethod*> m_marked_root_methods;
  // Concurrent stats
  std::atomic<size_t> m_consts_inserted{0};
  std::atomic<size_t> m_casts_inserted{0};
};

// Users should be talking to the singleton which is set up to be operational
// across the pass list.
WrappedPrimitives* get_instance();
void initialize(const std::vector<Spec>& wrapper_specs);

// Simple checks for other passes to see if state has been configured.
bool is_wrapped_api(const DexMethodRef* ref);

// Simplified entry point of optimizing a method, if configured.
void optimize_method(const TypeSystem& type_system,
                     const cp::intraprocedural::FixpointIterator& intra_cp,
                     const cp::WholeProgramState& wps,
                     DexMethod* method,
                     cfg::ControlFlowGraph& cfg);
} // namespace wrapped_primitives
