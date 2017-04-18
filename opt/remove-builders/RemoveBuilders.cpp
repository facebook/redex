/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveBuilders.h"

#include <boost/regex.hpp>
#include <tuple>

#include "Dataflow.h"
#include "DexUtil.h"
#include "RemoveBuildersHelper.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_CLASSES_REMOVED = "classes_removed";
constexpr const char* METRIC_FIELDS_REMOVED = "fields_removed";
constexpr const char* METRIC_METHODS_REMOVED = "methods_removed";
constexpr const char* METRIC_METHODS_CLEARED = "methods_cleared";

struct builder_counters {
  size_t classes_removed{0};
  size_t fields_removed{0};
  size_t methods_removed{0};
  size_t methods_cleared{0};
};

builder_counters b_counter;

bool has_builder_name(DexClass* cls) {
  always_assert(cls != nullptr);

  static boost::regex re{"\\$Builder;$"};
  return boost::regex_search(cls->c_str(), re);
}

DexType* get_buildee(DexType* builder) {
  always_assert(builder != nullptr);

  auto builder_name = std::string(builder->c_str());
  auto buildee_name = builder_name.substr(0, builder_name.size() - 9) + ";";
  return DexType::get_type(buildee_name.c_str());
}

void transfer_object_reach(DexType* obj,
                           uint16_t regs_size,
                           const IRInstruction* insn,
                           RegSet& regs) {
  always_assert(obj != nullptr);
  always_assert(insn != nullptr);

  auto op = insn->opcode();
  if (op == OPCODE_MOVE_OBJECT) {
    regs[insn->dest()] = regs[insn->src(0)];
  } else if (op == OPCODE_MOVE_RESULT) {
    regs[insn->dest()] = regs[regs_size];
  } else if (writes_result_register(op)) {
    if (is_invoke(op)) {
      auto invoked =
          static_cast<const IRMethodInstruction*>(insn)->get_method();
      if (invoked->get_proto()->get_rtype() == obj) {
        regs[regs_size] = 1;
        return;
      }
    }
    regs[regs_size] = 0;
  } else if (insn->dests_size() != 0) {
    regs[insn->dest()] = 0;
  }
}

bool tainted_reg_escapes(
    DexType* ty,
    const std::unordered_map<IRInstruction*, TaintedRegs>& taint_map) {
  always_assert(ty != nullptr);

  for (auto it : taint_map) {
    auto insn = it.first;
    auto tainted = it.second.bits();
    auto op = insn->opcode();
    if (is_invoke(insn->opcode())) {
      auto invoked =
          static_cast<const IRMethodInstruction*>(insn)->get_method();
      invoked = resolve_method(invoked, MethodSearch::Any);
      if (!invoked) {
        TRACE(BUILDERS, 5, "Unable to resolve %s\n", SHOW(insn));
      } else if (invoked->get_class() == ty &&
                 !(invoked->get_access() & ACC_STATIC)) {
        // TODO: check if we are actually passing the tainted register as the
        // `this` arg
        continue;
      }
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        if (tainted[insn->src(i)]) {
          TRACE(BUILDERS, 5, "Escaping instruction: %s\n", SHOW(insn));
          return true;
        }
      }
    } else if (op == OPCODE_SPUT_OBJECT || op == OPCODE_IPUT_OBJECT ||
               op == OPCODE_APUT_OBJECT || op == OPCODE_RETURN_OBJECT) {
      if (tainted[insn->src(0)]) {
        TRACE(BUILDERS, 5, "Escaping instruction: %s\n", SHOW(insn));
        return true;
      }
    }
  }
  return false;
}

// checks if the `this` argument on an instance method ever gets passed to
// a method that doesn't belong to the same instance, or if it gets stored
// in a field, or if it escapes as a return value.
bool this_arg_escapes(DexMethod* method) {
  always_assert(method != nullptr);
  always_assert(!(method->get_access() & ACC_STATIC));

  auto code = method->get_code();
  IRInstruction* first_insn = InstructionIterable(code).begin()->insn;
  auto regs_size = code->get_registers_size();
  auto this_reg = regs_size - code->get_ins_size();
  auto this_cls = method->get_class();
  code->build_cfg();
  auto blocks = postorder_sort(code->cfg().blocks());
  std::reverse(blocks.begin(), blocks.end());
  std::function<void(const IRInstruction*, TaintedRegs*)> trans = [&](
      const IRInstruction* insn, TaintedRegs* tregs) {
    auto& regs = tregs->m_reg_set;
    if (insn == first_insn) {
      regs[this_reg] = 1;
    }
    transfer_object_reach(this_cls, regs_size, insn, regs);
  };
  auto taint_map = forwards_dataflow(blocks, TaintedRegs(regs_size + 1), trans);
  return tainted_reg_escapes(this_cls, *taint_map);
}

bool this_arg_escapes(DexClass* cls) {
  always_assert(cls != nullptr);

  bool result = false;
  for (DexMethod* m : cls->get_dmethods()) {
    if (!m->get_code()) {
      continue;
    }
    if (!(m->get_access() & ACC_STATIC) && this_arg_escapes(m)) {
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
    if (this_arg_escapes(m)) {
      TRACE(BUILDERS,
            3,
            "this escapes in %s\n",
            m->get_deobfuscated_name().c_str());
      result = true;
    }
  }
  return result;
}

std::vector<DexMethod*> get_constructors(
    const std::vector<DexMethod*>& dmethods) {
  std::vector<DexMethod*> constructors;

  for (const auto& dmethod : dmethods) {
    if (is_init(dmethod)) {
      constructors.emplace_back(dmethod);
    }
  }

  return constructors;
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
 * Check builder's constructor does a small amount of work
 *  - instantiates the parent class (Object)
 *  - returns
 */
bool is_trivial_builder_constructor(DexMethod* method) {
  always_assert(method != nullptr);

  const auto* code = method->get_code();
  if (!code) {
    return false;
  }

  if (!is_constructor(method)) {
    return false;
  }

  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  static auto init = DexString::make_string("<init>");
  if (it->insn->opcode() != OPCODE_INVOKE_DIRECT) {
    return false;
  } else {
    auto invoked =
        static_cast<const IRMethodInstruction*>(it->insn)->get_method();
    if (invoked->get_name() != init) {
      return false;
    }
  }

  ++it;
  if (it->insn->opcode() != OPCODE_RETURN_VOID) {
    return false;
  }
  ++it;
  if (it != ii.end()) {
    return false;
  }

  return true;
}

/**
 * First pass through what "trivial builder" means:
 *  - is a builder
 *  - it doesn't escape stack
 *  - has no static methods
 *  - has no static fields
 *
 * TODO(emmasevastian): Extend the "definition".
 */
std::unordered_set<DexClass*> get_trivial_builders(
    const std::unordered_set<DexClass*>& builder_classes,
    const std::unordered_set<DexType*>& stack_only_builders) {

  std::unordered_set<DexClass*> trivial_builders;

  for (DexClass* builder_class : builder_classes) {
    DexType* builder_type = builder_class->get_type();

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

    // Filter out builders that do extra work in the constructor.
    std::vector<DexMethod*> builder_constructors =
        get_constructors(builder_class->get_dmethods());
    if (builder_constructors.size() != 1 ||
        !is_trivial_builder_constructor(builder_constructors[0])) {
      continue;
    }

    trivial_builders.emplace(builder_class);
  }

  return trivial_builders;
}

void remove_builder_classes(const std::unordered_set<DexClass*>& builder,
                            const std::unordered_set<DexClass*>& kept_builders,
                            Scope& classes) {

  std::unordered_set<DexClass*> class_references;
  for (const auto& cls : classes) {
    DexType* super_type = cls->get_super_class();
    if (!super_type) {
      continue;
    }

    DexClass* super_cls = type_class(super_type);
    if (!super_cls) {
      continue;
    }

    if (has_builder_name(super_cls)) {
      class_references.insert(super_cls);
    }
  }

  classes.erase(
      remove_if(classes.begin(),
                classes.end(),
                [&](DexClass* cls) {
                  if (class_references.find(cls) != class_references.end()) {
                    return false;
                  }

                  if (builder.find(cls) != builder.end() &&
                      kept_builders.find(cls) == kept_builders.end()) {

                    b_counter.classes_removed++;
                    b_counter.methods_removed +=
                        cls->get_vmethods().size() + cls->get_dmethods().size();
                    b_counter.fields_removed += cls->get_ifields().size();

                    return true;
                  }

                  return false;
                }),
      classes.end());
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
      DexType* cls = static_cast<IRTypeInstruction*>(insn)->get_type();
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
  code->build_cfg();
  auto blocks = postorder_sort(code->cfg().blocks());
  std::reverse(blocks.begin(), blocks.end());
  auto regs_size = method->get_code()->get_registers_size();
  std::function<void(const IRInstruction*, TaintedRegs*)> trans = [&](
      const IRInstruction* insn, TaintedRegs* tregs) {
    auto& regs = tregs->m_reg_set;
    auto op = insn->opcode();
    if (op == OPCODE_NEW_INSTANCE) {
      DexType* cls = static_cast<const IRTypeInstruction*>(insn)->get_type();
      if (cls == builder) {
        regs[insn->dest()] = 1;
      }
    } else {
      transfer_object_reach(builder, regs_size, insn, regs);
    }
  };
  auto taint_map = forwards_dataflow(blocks, TaintedRegs(regs_size + 1), trans);
  return tainted_reg_escapes(builder, *taint_map);
}

void RemoveBuildersPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles&,
                                  PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(BUILDERS,
          1,
          "RemoveBuildersPass did not run because no Proguard configuration "
          "was provided.");
    return;
  }

  std::unordered_set<DexClass*> builder_classes;

  auto scope = build_class_scope(stores);
  for (DexClass* cls : scope) {
    if (is_annotation(cls) || is_interface(cls)) {
      continue;
    }

    if (has_builder_name(cls)) {
      m_builders.emplace(cls->get_type());
      builder_classes.emplace(cls);
    }
  }

  std::unordered_set<DexType*> escaped_builders;
  walk_methods(scope, [&](DexMethod* m) {
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
  auto obj_type = get_object_type();
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
    if (cls->is_external() || this_arg_escapes(cls)) {
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
      get_trivial_builders(builder_classes, no_escapes);

  std::vector<std::tuple<DexMethod*, DexClass*>> method_to_inlined_builders;
  std::unordered_set<DexClass*> kept_builders;

  PassConfig pc(mgr.get_config());
  BuilderTransform b_transform(pc, scope, stores[0].get_dexen()[0]);

  // Inline build methods.
  walk_methods(scope, [&](DexMethod* method) {
    auto builders = created_builders(method);

    for (DexType* builder : builders) {
      DexClass* builder_cls = type_class(builder);
      // Check it is a trivial one.
      if (trivial_builders.find(builder_cls) != trivial_builders.end()) {
        if (!b_transform.inline_builder_methods(method, builder_cls)) {
          kept_builders.emplace(builder_cls);
        } else {
          method_to_inlined_builders.emplace_back(method, builder_cls);
        }
      }
    }
  });

  for (const auto& pair : method_to_inlined_builders) {
    DexMethod* method = std::get<0>(pair);
    DexClass* builder = std::get<1>(pair);

    DexType* buildee = get_buildee(builder->get_type());
    always_assert(buildee != nullptr);

    if (!remove_builder(method, builder, type_class(buildee))) {
      kept_builders.emplace(builder);
    } else {
      b_counter.methods_cleared++;
    }
  }

  remove_builder_classes(trivial_builders, kept_builders, scope);
  post_dexen_changes(scope, stores);

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
  TRACE(BUILDERS, 1, "Stats for unescaping builders:\n", dmethod_count);
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
