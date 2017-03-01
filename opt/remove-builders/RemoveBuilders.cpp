/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/dynamic_bitset.hpp>
#include <boost/regex.hpp>

#include "Dataflow.h"
#include "DexUtil.h"
#include "RemoveBuilders.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

bool has_builder_name(DexClass* cls) {
  static boost::regex re {"\\$Builder;$"};
  return boost::regex_search(cls->c_str(), re);
}

DexType* get_buildee(DexType* builder) {
  auto builder_name = std::string(builder->c_str());
  auto buildee_name = builder_name.substr(0, builder_name.size() - 9) + ";";
  return DexType::get_type(buildee_name.c_str());
}

using RegSet = boost::dynamic_bitset<>;

struct TaintedRegs {
  RegSet m_reg_set;

  explicit TaintedRegs(int nregs): m_reg_set(nregs) {}
  explicit TaintedRegs(const RegSet&& reg_set)
      : m_reg_set(std::move(reg_set)) {}

  const RegSet& bits() { return m_reg_set; }

  void meet(const TaintedRegs& that) {
    m_reg_set |= that.m_reg_set;
  }
  void trans(const DexInstruction*);
  bool operator==(const TaintedRegs& that) const {
    return m_reg_set == that.m_reg_set;
  }
  bool operator!=(const TaintedRegs& that) const {
    return !(*this == that);
  }
};


void transfer_object_reach(DexType* obj,
                           uint16_t regs_size,
                           const DexInstruction* insn,
                           RegSet& regs) {
  auto op = insn->opcode();
  if (op == OPCODE_MOVE_OBJECT) {
    regs[insn->dest()] = regs[insn->src(0)];
  } else if (op == OPCODE_MOVE_RESULT) {
    regs[insn->dest()] = regs[regs_size];
  } else if (writes_result_register(op)) {
    if (is_invoke(op)) {
      auto invoked = static_cast<const DexOpcodeMethod*>(insn)->get_method();
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
    const std::unordered_map<DexInstruction*, TaintedRegs>& taint_map) {
  for (auto it : taint_map) {
    auto insn = it.first;
    auto tainted = it.second.bits();
    auto op = insn->opcode();
    if (is_invoke(insn->opcode())) {
      auto invoked = static_cast<const DexOpcodeMethod*>(insn)->get_method();
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
  always_assert(!(method->get_access() & ACC_STATIC));
  auto& code = method->get_code();
  DexInstruction* first_insn = code->get_instructions().at(0);
  auto regs_size = code->get_registers_size();
  auto this_reg = regs_size - code->get_ins_size();
  auto this_cls = method->get_class();
  MethodTransformer mt(method, /* want_cfg */ true);
  auto blocks = postorder_sort(mt->cfg());
  std::reverse(blocks.begin(), blocks.end());
  std::function<void(const DexInstruction*, TaintedRegs*)> trans = [&](
      const DexInstruction* insn, TaintedRegs* tregs) {
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
  bool result = false;
  for (DexMethod* m : cls->get_dmethods()) {
    if (!m->get_code()) {
      continue;
    }
    if (!(m->get_access() & ACC_STATIC) &&
        this_arg_escapes(m)) {
      TRACE(BUILDERS, 3, "this escapes in %s\n",
            m->get_deobfuscated_name().c_str());
      result = true;
    }
  }
  for (DexMethod* m : cls->get_vmethods()) {
    if (!m->get_code()) {
      continue;
    }
    if (this_arg_escapes(m)) {
      TRACE(BUILDERS, 3, "this escapes in %s\n",
            m->get_deobfuscated_name().c_str());
      result = true;
    }
  }
  return result;
}

bool is_trivial_setter(DexMethod* m) {
  auto& code = m->get_code();
  if (!code) {
    return false;
  }
  auto& insns = code->get_instructions();
  if (insns.size() != 2) {
    return false;
  }
  if (is_iput(insns[0]->opcode()) &&
      insns[0]->src(0) == 1 &&
      insns[0]->src(1) == 0 &&
      insns[1]->opcode() == OPCODE_RETURN_OBJECT &&
      insns[1]->src(0) == 0) {
    return true;
  }
  TRACE(BUILDERS, 5, "Not quite a setter: %s\n", SHOW(code));
  return false;
}

std::vector<DexMethod*> get_static_methods(std::vector<DexMethod*>& dmethods) {
  std::vector<DexMethod*> static_methods;

  for (const auto& dmethod: dmethods) {
    if (is_static(dmethod)) {
      static_methods.emplace_back(dmethod);
    }
  }

  return static_methods;
}

std::vector<DexMethod*> get_private_methods(std::vector<DexMethod*>& dmethods) {
  std::vector<DexMethod*> private_methods;

  for (const auto& dmethod: dmethods) {
    if (!is_constructor(dmethod) && !is_static(dmethod)) {
      private_methods.emplace_back(dmethod);
    }
  }

  return private_methods;
}

DexMethod* get_build_method(std::vector<DexMethod*>& vmethods) {
  for (const auto& vmethod : vmethods) {
    if (strcmp(vmethod->c_str(), "build") == 0) {
      return vmethod;
    }
  }

  return nullptr;
}

/**
 * Checks build method does a small amount of work
 *   - returns an instance of enclosing class type
 *   - only that instance is created
 */
bool is_trivial_build_method(DexMethod* method, DexType* cls_type) {
  // Check it returns an instance of the class it was defined in.
  auto proto = method->get_proto();
  auto return_type = proto->get_rtype();
  if (strcmp(cls_type->c_str(), return_type->c_str()) != 0) {
    return false;
  }

  // Check there is only one instance created.
  const auto& code = method->get_code();
  const auto& insns = code->get_instructions();
  int instances = 0;

  for (DexInstruction* insn : insns) {
    if (insn->opcode() == OPCODE_NEW_INSTANCE) {
      instances++;
    }
  }

  return instances == 1;
}

/**
 * Basic check for number of instance fields.
 *
 * TODO(emmasevastian); Check relationship between fields.
 */
bool have_same_instance_fields(DexClass* builder, DexClass* cls) {
  return builder->get_ifields().size() == cls->get_ifields().size();
}

/**
 * First pass through what "trivial builder" means:
 *  - is a builder
 *  - it doesn't escape stack
 *  - it has only one vmethod: the build method
 *  - the build method only calls the constructor of the class it is defined in
 *  - has no private or static method
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
    bool has_private_methods =
      get_private_methods(builder_class->get_dmethods()).size() != 0;
    DexMethod* build_method = get_build_method(builder_class->get_vmethods());

    if (has_static_methods ||
        has_private_methods ||
        build_method == nullptr ||
        builder_class->get_vmethods().size() > 1) {
      continue;
    }

    DexType* buildee_type = get_buildee(builder_class->get_type());
    // Filter out builders that do "extra work" in the build method.
    if (!is_trivial_build_method(build_method, buildee_type)) {
      continue;
    }

    // Filter out builders that have extra instance fields.
    if (!have_same_instance_fields(builder_class, type_class(buildee_type))) {
      continue;
    }

    trivial_builders.emplace(builder_class);
  }

  return trivial_builders;
}

} // namespace

std::vector<DexType*> RemoveBuildersPass::created_builders(DexMethod* m) {
  std::vector<DexType*> builders;
  auto& code = m->get_code();
  if (!code) {
    return builders;
  }
  for (DexInstruction* insn : code->get_instructions()) {
    if (insn->opcode() == OPCODE_NEW_INSTANCE) {
      DexType* cls = static_cast<DexOpcodeType*>(insn)->get_type();
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
  MethodTransformer mt(method, /* want_cfg */ true);
  auto blocks = postorder_sort(mt->cfg());
  std::reverse(blocks.begin(), blocks.end());
  auto regs_size = method->get_code()->get_registers_size();
  std::function<void(const DexInstruction*, TaintedRegs*)> trans = [&](
      const DexInstruction* insn, TaintedRegs* tregs) {
    auto& regs = tregs->m_reg_set;
    auto op = insn->opcode();
    if (op == OPCODE_NEW_INSTANCE) {
      DexType* cls = static_cast<const DexOpcodeType*>(insn)->get_type();
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


void RemoveBuildersPass::run_pass(
  DexStoresVector& stores,
  ConfigFiles&,
  PassManager& mgr
) {
  std::unordered_set<DexClass*> builder_classes;

  auto scope = build_class_scope(stores);
  for (DexClass* cls : scope) {
    if (has_builder_name(cls) && !is_interface(cls)) {
      m_builders.emplace(cls->get_type());
      builder_classes.emplace(cls);
    }
  }

  std::unordered_set<DexType*> escaped_builders;
  walk_methods(scope, [&](DexMethod* m) {
    auto builders = created_builders(m);
    for (DexType* builder : builders) {
      if (escapes_stack(builder, m)) {
        TRACE(BUILDERS, 3, "%s escapes in %s\n",
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
  size_t setter_count = 0;
  size_t build_count = 0;
  for (DexType* builder : no_escapes) {
    auto cls = type_class(builder);
    auto buildee = get_buildee(builder);
    dmethod_count += cls->get_dmethods().size();
    vmethod_count += cls->get_vmethods().size();
    for (DexMethod* m : cls->get_vmethods()) {
      if (m->get_proto()->get_rtype() == buildee) {
        build_count++;
      } else if (is_trivial_setter(m)) {
        setter_count++;
      }
    }
  }

  mgr.set_metric("total_builders", m_builders.size());
  mgr.set_metric("stack_only_builders", stack_only_builders.size());
  mgr.set_metric("no_escapes", no_escapes.size());

  TRACE(BUILDERS, 1, "Total builders: %d\n", m_builders.size());
  TRACE(BUILDERS, 1, "Stack-only builders: %d\n", stack_only_builders.size());
  TRACE(BUILDERS, 1, "Stack-only builders that don't let `this` escape: %d\n",
        no_escapes.size());
  TRACE(BUILDERS, 1, "Stats for unescaping builders:\n", dmethod_count);
  TRACE(BUILDERS, 1, "\tdmethods: %d\n", dmethod_count);
  TRACE(BUILDERS, 1, "\tvmethods: %d\n", vmethod_count);
  TRACE(BUILDERS, 1, "\ttrivial setters: %d\n", setter_count);
  TRACE(BUILDERS, 1, "\tbuild methods: %d\n", build_count);

  std::unordered_set<DexClass*> trivial_builders = get_trivial_builders(
      builder_classes, stack_only_builders);
  TRACE(BUILDERS, 1, "Trivial builders: %d\n", trivial_builders.size());
}

static RemoveBuildersPass s_pass;
