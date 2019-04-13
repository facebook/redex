/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveBuilders.h"

#include <boost/regex.hpp>
#include <tuple>

#include "Dataflow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RemoveBuildersHelper.h"
#include "Resolver.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_CLASSES_REMOVED = "classes_removed";
constexpr const char* METRIC_FIELDS_REMOVED = "fields_removed";
constexpr const char* METRIC_METHODS_REMOVED = "methods_removed";
constexpr const char* METRIC_METHODS_CLEARED = "methods_cleared";

struct builder_counters {
  size_t classes_removed;
  size_t fields_removed;
  size_t methods_removed;
  size_t methods_cleared;
};

builder_counters b_counter;

// checks if the `this` argument on an instance method ever gets passed to
// a method that doesn't belong to the same instance, or if it gets stored
// in a field, or if it escapes as a return value.
bool this_arg_escapes(DexMethod* method, bool enable_buildee_constr_change) {
  always_assert(method != nullptr);
  always_assert(!(method->get_access() & ACC_STATIC));

  auto code = method->get_code();
  auto ii = InstructionIterable(code);
  auto this_insn = ii.begin()->insn;
  always_assert(this_insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT);
  auto regs_size = code->get_registers_size();
  auto this_cls = method->get_class();
  code->build_cfg(/* editable */ false);
  auto blocks = cfg::postorder_sort(code->cfg().blocks());
  std::reverse(blocks.begin(), blocks.end());
  std::function<void(IRList::iterator, TaintedRegs*)> trans =
      [&](IRList::iterator it, TaintedRegs* tregs) {
        auto* insn = it->insn;
        if (insn == this_insn) {
          tregs->m_reg_set[insn->dest()] = 1;
        } else {
          transfer_object_reach(this_cls, regs_size, insn, tregs->m_reg_set);
        }
      };
  auto taint_map = forwards_dataflow(blocks, TaintedRegs(regs_size + 1), trans);
  return tainted_reg_escapes(
      this_cls, method, *taint_map, enable_buildee_constr_change);
}

bool this_arg_escapes(DexClass* cls, bool enable_buildee_constr_change) {
  always_assert(cls != nullptr);

  bool result = false;
  for (DexMethod* m : cls->get_dmethods()) {
    if (!m->get_code()) {
      continue;
    }
    if (!(m->get_access() & ACC_STATIC) &&
        this_arg_escapes(m, enable_buildee_constr_change)) {
      TRACE(BUILDERS,
            3,
            "this escapes in %s\n",
            m->get_deobfuscated_name().c_str());
      result = true;
    }
  }
  for (DexMethod* m : cls->get_vmethods()) {
    if (!m->get_code()) {
      continue;
    }
    if (this_arg_escapes(m, enable_buildee_constr_change)) {
      TRACE(BUILDERS,
            3,
            "this escapes in %s\n",
            m->get_deobfuscated_name().c_str());
      result = true;
    }
  }
  return result;
}

std::vector<DexMethod*> get_static_methods(
    const std::vector<DexMethod*>& dmethods) {
  std::vector<DexMethod*> static_methods;

  for (const auto& dmethod : dmethods) {
    if (is_static(dmethod)) {
      static_methods.emplace_back(dmethod);
    }
  }

  return static_methods;
}

/**
 * First pass through what "trivial builder" means:
 *  - is a builder
 *  - it doesn't escape stack
 *  - has no static methods
 *  - has no static fields
 */
std::unordered_set<DexClass*> get_trivial_builders(
    const std::unordered_set<DexType*>& builders,
    const std::unordered_set<DexType*>& stack_only_builders) {

  std::unordered_set<DexClass*> trivial_builders;

  for (DexType* builder_type : builders) {
    DexClass* builder_class = type_class(builder_type);

    // Filter out builders that escape the stack.
    if (stack_only_builders.find(builder_type) == stack_only_builders.end()) {
      continue;
    }

    // Filter out builders that do "extra work".
    bool has_static_methods =
        get_static_methods(builder_class->get_dmethods()).size() != 0;

    if (has_static_methods || builder_class->get_sfields().size()) {
      continue;
    }

    DexType* buildee_type = get_buildee(builder_class->get_type());
    if (!buildee_type) {
      continue;
    }

    trivial_builders.emplace(builder_class);
  }

  return trivial_builders;
}

void gather_removal_builder_stats(
    const std::unordered_set<DexClass*>& builders,
    const std::unordered_set<DexClass*>& kept_builders) {

  for (DexClass* builder : builders) {
    if (kept_builders.find(builder) == kept_builders.end()) {
      b_counter.classes_removed++;
      b_counter.methods_removed +=
          builder->get_vmethods().size() + builder->get_dmethods().size();
      b_counter.fields_removed += builder->get_ifields().size();
    }
  }
}

std::unordered_set<DexClass*> get_builders_with_subclasses(Scope& classes) {
  std::unordered_set<DexClass*> builders_with_subclasses;

  for (const auto& cls : classes) {
    DexType* super_type = cls->get_super_class();
    if (!super_type) {
      continue;
    }

    DexClass* super_cls = type_class(super_type);
    if (!super_cls) {
      continue;
    }

    if (has_builder_name(super_type)) {
      builders_with_subclasses.emplace(super_cls);
    }
  }

  return builders_with_subclasses;
}

} // namespace

std::vector<DexType*> RemoveBuildersPass::created_builders(DexMethod* m) {
  always_assert(m != nullptr);

  std::vector<DexType*> builders;
  auto code = m->get_code();
  if (!code) {
    return builders;
  }
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_NEW_INSTANCE) {
      DexType* cls = insn->get_type();
      if (m_builders.find(cls) != m_builders.end()) {
        builders.emplace_back(cls);
      }
    }
  }
  return builders;
}

// checks if any instances of :builder that get created in the method ever get
// passed to a method (aside from when its own instance methods get invoked),
// or if they get stored in a field, or if they escape as a return value.
bool RemoveBuildersPass::escapes_stack(DexType* builder, DexMethod* method) {
  always_assert(builder != nullptr);
  always_assert(method != nullptr);

  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto blocks = cfg::postorder_sort(code->cfg().blocks());
  std::reverse(blocks.begin(), blocks.end());
  auto regs_size = method->get_code()->get_registers_size();
  auto taint_map = get_tainted_regs(regs_size, blocks, builder);
  return tainted_reg_escapes(
      builder, method, *taint_map, m_enable_buildee_constr_change);
}

void RemoveBuildersPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& /* conf */,
                                  PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(BUILDERS,
          1,
          "RemoveBuildersPass did not run because no Proguard configuration "
          "was provided.");
    return;
  }

  // Initialize couters.
  b_counter = {0, 0, 0, 0};

  auto obj_type = get_object_type();
  auto scope = build_class_scope(stores);
  for (DexClass* cls : scope) {
    if (is_annotation(cls) || is_interface(cls) ||
        cls->get_super_class() != obj_type) {
      continue;
    }

    if (has_builder_name(cls->get_type())) {
      m_builders.emplace(cls->get_type());
    }
  }

  std::unordered_set<DexType*> escaped_builders;
  walk::methods(scope, [&](DexMethod* m) {
    auto builders = created_builders(m);
    for (DexType* builder : builders) {
      if (escapes_stack(builder, m)) {
        TRACE(BUILDERS,
              3,
              "%s escapes in %s\n",
              SHOW(builder),
              m->get_deobfuscated_name().c_str());
        escaped_builders.emplace(builder);
      }
    }
  });

  std::unordered_set<DexType*> stack_only_builders;
  for (DexType* builder : m_builders) {
    if (escaped_builders.find(builder) == escaped_builders.end()) {
      stack_only_builders.emplace(builder);
    }
  }

  std::unordered_set<DexType*> builders_and_supers;
  for (DexType* builder : stack_only_builders) {
    DexType* cls = builder;
    while (cls != nullptr && cls != obj_type) {
      builders_and_supers.emplace(cls);
      cls = type_class(cls)->get_super_class();
    }
  }

  std::unordered_set<DexType*> this_escapes;
  for (DexType* cls_ty : builders_and_supers) {
    DexClass* cls = type_class(cls_ty);
    if (cls->is_external() ||
        this_arg_escapes(cls, m_enable_buildee_constr_change)) {
      this_escapes.emplace(cls_ty);
    }
  }

  // set of builders that neither escape the stack nor pass their 'this' arg
  // to another function
  std::unordered_set<DexType*> no_escapes;
  for (DexType* builder : stack_only_builders) {
    DexType* cls = builder;
    bool hierarchy_has_escape = false;
    while (cls != nullptr) {
      if (this_escapes.find(cls) != this_escapes.end()) {
        hierarchy_has_escape = true;
        break;
      }
      cls = type_class(cls)->get_super_class();
    }
    if (!hierarchy_has_escape) {
      no_escapes.emplace(builder);
    }
  }

  size_t dmethod_count = 0;
  size_t vmethod_count = 0;
  size_t build_count = 0;
  for (DexType* builder : no_escapes) {
    auto cls = type_class(builder);
    auto buildee = get_buildee(builder);
    dmethod_count += cls->get_dmethods().size();
    vmethod_count += cls->get_vmethods().size();
    for (DexMethod* m : cls->get_vmethods()) {
      if (m->get_proto()->get_rtype() == buildee) {
        build_count++;
      }
    }
  }

  std::unordered_set<DexClass*> trivial_builders =
      get_trivial_builders(m_builders, no_escapes);

  std::unordered_set<DexClass*> kept_builders =
      get_builders_with_subclasses(scope);

  BuilderTransform b_transform(scope, stores, false);

  // Inline non init methods.
  std::unordered_set<DexClass*> removed_builders;
  walk::methods(scope, [&](DexMethod* method) {
    auto builders = created_builders(method);

    for (DexType* builder : builders) {
      if (method->get_class() == builder) {
        continue;
      }

      DexClass* builder_cls = type_class(builder);

      // Filter out builders that we cannot remove.
      if (kept_builders.find(builder_cls) != kept_builders.end()) {
        continue;
      }

      // Check it is a trivial one.
      if (trivial_builders.find(builder_cls) != trivial_builders.end()) {

        DexMethod* method_copy = DexMethod::make_method_from(
            method,
            method->get_class(),
            DexString::make_string(method->get_name()->str() +
                                   "$redex_builders"));
        bool was_not_removed =
            !b_transform.inline_methods(
                method, builder, &get_non_init_methods) ||
            !remove_builder_from(method, builder_cls, b_transform);

        if (was_not_removed) {
          kept_builders.emplace(builder_cls);
          method->set_code(method_copy->release_code());
        } else {
          b_counter.methods_cleared++;
          removed_builders.emplace(builder_cls);
        }

        DexMethod::erase_method(method_copy);
      }
    }
  });

  // No need to remove the builders here, since `RemoveUnreachable` will
  // take care of it.
  gather_removal_builder_stats(removed_builders, kept_builders);

  mgr.set_metric("total_builders", m_builders.size());
  mgr.set_metric("stack_only_builders", stack_only_builders.size());
  mgr.set_metric("no_escapes", no_escapes.size());
  mgr.incr_metric(METRIC_CLASSES_REMOVED, b_counter.classes_removed);
  mgr.incr_metric(METRIC_METHODS_REMOVED, b_counter.methods_removed);
  mgr.incr_metric(METRIC_FIELDS_REMOVED, b_counter.fields_removed);
  mgr.incr_metric(METRIC_METHODS_CLEARED, b_counter.methods_cleared);

  TRACE(BUILDERS, 1, "Total builders: %d\n", m_builders.size());
  TRACE(BUILDERS, 1, "Stack-only builders: %d\n", stack_only_builders.size());
  TRACE(BUILDERS,
        1,
        "Stack-only builders that don't let `this` escape: %d\n",
        no_escapes.size());
  TRACE(BUILDERS, 1, "Stats for unescaping builders:\n");
  TRACE(BUILDERS, 1, "\tdmethods: %d\n", dmethod_count);
  TRACE(BUILDERS, 1, "\tvmethods: %d\n", vmethod_count);
  TRACE(BUILDERS, 1, "\tbuild methods: %d\n", build_count);
  TRACE(BUILDERS, 1, "Trivial builders: %d\n", trivial_builders.size());
  TRACE(BUILDERS, 1, "Classes removed: %d\n", b_counter.classes_removed);
  TRACE(BUILDERS, 1, "Methods removed: %d\n", b_counter.methods_removed);
  TRACE(BUILDERS, 1, "Fields removed: %d\n", b_counter.fields_removed);
  TRACE(BUILDERS, 1, "Methods cleared: %d\n", b_counter.methods_cleared);
}

static RemoveBuildersPass s_pass;
