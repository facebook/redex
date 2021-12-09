/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BuilderTransform.h"

#include "DexClass.h"
#include "IRCode.h"
#include "Inliner.h"
#include "RemoveBuilderPattern.h"

namespace builder_pattern {

BuilderTransform::BuilderTransform(
    const Scope& scope,
    const TypeSystem& type_system,
    const DexType* root,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const inliner::InlinerConfig& inliner_config,
    DexStoresVector& stores)
    : m_type_system(type_system),
      m_root(root),
      m_inliner_config(inliner_config) {
  auto concurrent_resolver = [&](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, m_concurrent_resolved_refs);
  };

  std::unordered_set<DexMethod*> no_default_inlinables;
  // customize shrinking options
  m_inliner_config.shrinker = shrinker::ShrinkerConfig();
  m_inliner_config.shrinker.run_const_prop = true;
  m_inliner_config.shrinker.run_cse = true;
  m_inliner_config.shrinker.run_copy_prop = true;
  m_inliner_config.shrinker.run_local_dce = true;
  m_inliner_config.shrinker.compute_pure_methods = false;
  m_inliner = std::unique_ptr<MultiMethodInliner>(new MultiMethodInliner(
      scope, init_classes_with_side_effects, stores, no_default_inlinables,
      concurrent_resolver, m_inliner_config, MultiMethodInlinerMode::None));
}

std::unordered_set<const IRInstruction*>
BuilderTransform::get_not_inlined_insns(
    DexMethod* caller, const std::unordered_set<IRInstruction*>& insns) {
  always_assert(caller && caller->get_code());
  // TODO: We are going to leak instructions when we are successful. Change this
  // to true after making the pass aware of memory ownership.
  bool delete_removed_insns = false;
  m_inliner->inline_callees(caller, insns, delete_removed_insns);
  std::unordered_set<const IRInstruction*> not_inlined_insns;
  // Check if everything was inlined.
  auto* code = caller->get_code();
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insns.count(insn)) {
      not_inlined_insns.emplace(insn);
    }
  }

  return not_inlined_insns;
}

BuilderTransform::~BuilderTransform() {}

/**
 * For all the methods of the given type, try inlining all super calls and
 * constructors of the super type. If any of them fails, return false.
 */
bool BuilderTransform::inline_super_calls_and_ctors(const DexType* type) {
  auto cls = type_class(type);

  std::vector<DexMethod*> methods = cls->get_dmethods();
  const std::vector<DexMethod*>& vmethods = cls->get_vmethods();
  methods.insert(methods.end(), vmethods.begin(), vmethods.end());

  const std::vector<DexMethod*>& super_ctors_list =
      type_class(m_root)->get_ctors();
  std::unordered_set<DexMethod*> super_ctors(super_ctors_list.begin(),
                                             super_ctors_list.end());

  for (DexMethod* method : methods) {
    if (!method->get_code()) {
      continue;
    }

    std::unordered_set<IRInstruction*> inlinable_insns;
    for (const auto& mie : InstructionIterable(method->get_code())) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_INVOKE_SUPER) {
        inlinable_insns.emplace(insn);
      } else if (opcode::is_invoke_direct(insn->opcode())) {
        auto callee = resolve_method(insn->get_method(), MethodSearch::Direct);
        if (super_ctors.count(callee)) {
          inlinable_insns.emplace(insn);
        }
      }
    }

    if (!inlinable_insns.empty()) {
      TRACE(BLD_PATTERN, 8, "Creating a copy of %s", SHOW(method));

      const std::string& name_str = method->get_name()->str();
      DexMethod* method_copy = DexMethod::make_method_from(
          method,
          method->get_class(),
          DexString::make_string(name_str + "$redex_builder"));
      m_method_copy[method] = method_copy;

      size_t num_insns_not_inlined =
          get_not_inlined_insns(method, inlinable_insns).size();
      if (num_insns_not_inlined > 0) {
        return false;
      }
    }
  }

  return true;
}

/**
 * Bind virtual calls to the actual implementation.
 */
void BuilderTransform::update_virtual_calls(
    const std::unordered_map<IRInstruction*, DexType*>& insn_to_type) {

  for (const auto& pair : insn_to_type) {
    auto insn = pair.first;
    auto current_instance = pair.second;

    if (opcode::is_invoke_virtual(insn->opcode())) {
      auto method = resolve_method(insn->get_method(), MethodSearch::Virtual);
      if (!method) {
        continue;
      }

      if (method->get_class() == m_root) {
        // replace it with the actual implementation if any provided.
        auto virtual_scope = m_type_system.find_virtual_scope(method);
        for (const auto& v_pair : virtual_scope->methods) {
          auto m = v_pair.first;
          if (m->get_class() == current_instance && m->is_def()) {
            TRACE(BLD_PATTERN, 3,
                  "Replace virtual method %s with the current implementation "
                  "%s",
                  SHOW(method), SHOW(m));
            insn->set_method(m);
            break;
          }
        }
      }
    }
  }
}

namespace {

void initialize_regs(
    const std::map<DexField*, size_t, dexfields_comparator>& field_to_reg,
    IRCode* code) {

  auto params = code->get_param_instructions();

  for (const auto& pair : field_to_reg) {
    auto field = pair.first;
    auto reg = pair.second;

    IRInstruction* initialization_insn = nullptr;
    if (type::is_wide_type(field->get_type())) {
      initialization_insn = new IRInstruction(OPCODE_CONST_WIDE);
    } else {
      initialization_insn = new IRInstruction(OPCODE_CONST);
    }
    initialization_insn->set_dest(reg);
    initialization_insn->set_literal(0);
    code->insert_before(params.end(), initialization_insn);
  }
}

} // namespace

void BuilderTransform::replace_fields(const InstantiationToUsage& usage,
                                      DexMethod* method) {
  auto code = method->get_code();

  std::unordered_set<const IRInstruction*> deletes;
  std::unordered_map<IRInstruction*, IRInstruction*> replacement;

  for (const auto& mie : InstructionIterable(code)) {
    auto instantiation_insn = mie.insn;
    if (!usage.count(instantiation_insn)) {
      continue;
    }

    always_assert_log(instantiation_insn->opcode() == OPCODE_NEW_INSTANCE,
                      "Only accept new_instance opcodes for builder "
                      "initializations, but got %s\n",
                      SHOW(instantiation_insn));

    // Replace builder instance creation with Object creation.
    // This value should be only used for comparison with NULL.
    instantiation_insn->set_type(type::java_lang_Object());

    std::map<DexField*, size_t, dexfields_comparator> field_to_reg;
    for (const auto& insn : usage.at(instantiation_insn)) {

      if (opcode::is_an_iput(insn->opcode()) ||
          opcode::is_an_iget(insn->opcode())) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
        always_assert(field);

        if (field_to_reg.count(field) == 0) {
          field_to_reg[field] = type::is_wide_type(field->get_type())
                                    ? code->allocate_wide_temp()
                                    : code->allocate_temp();
        }

        // Replace usage.
        IRInstruction* new_insn = nullptr;
        if (type::is_wide_type(field->get_type())) {
          new_insn = new IRInstruction(OPCODE_MOVE_WIDE);
        } else if (type::is_primitive(field->get_type())) {
          new_insn = new IRInstruction(OPCODE_MOVE);
        } else {
          new_insn = new IRInstruction(OPCODE_MOVE_OBJECT);
        }

        if (opcode::is_an_iput(insn->opcode())) {
          new_insn->set_dest(field_to_reg[field]);
          new_insn->set_src(0, insn->src(0));
        } else {
          // Get the destination register from the next instruction.
          // TODO(emmasevastian): Keep track of the iterator instead of
          //                      the instruction.
          auto ii = InstructionIterable(*code);
          for (auto it = ii.begin(); it != ii.end(); ++it) {
            if (it->insn == insn) {
              new_insn->set_dest(std::next(it)->insn->dest());
              break;
            }
          }
          new_insn->set_src(0, field_to_reg[field]);
        }
        replacement[const_cast<IRInstruction*>(insn)] = new_insn;
      } else if (insn->opcode() == OPCODE_MOVE_OBJECT ||
                 insn->opcode() == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT ||
                 opcode::is_a_conditional_branch(insn->opcode())) {
        // Keep these instructions as is because we might not be able to clean
        // up all the paths where Object created instead of builder could
        // be used for checking if the builder was created or not.
        // ConstProp + DCE will take care of safe clean up for us.
      } else {
        always_assert_log(insn->opcode() == OPCODE_INVOKE_DIRECT ||
                              insn->opcode() == OPCODE_CHECK_CAST,
                          "Different insn %s", SHOW(insn));
        if (insn->opcode() == OPCODE_INVOKE_DIRECT) {
          auto invoked =
              resolve_method(insn->get_method(), MethodSearch::Direct);

          // We only accept `Object.<init>()` here, since we can't inline it
          // any further. Keep Object.<init> inplace to avoid confusing dex2oat.
          always_assert(invoked->get_class() == type::java_lang_Object() &&
                        method::is_init(invoked));
        } else {
          deletes.emplace(insn);
        }
      }
    }

    initialize_regs(field_to_reg, code);
  }

  for (const auto* insn : deletes) {
    code->remove_opcode(const_cast<IRInstruction*>(insn));
  }

  for (const auto& pair : replacement) {
    code->replace_opcode(pair.first, pair.second);
  }
}

void BuilderTransform::cleanup() {
  for (const auto& pair : m_method_copy) {
    auto method = pair.first;
    auto copy = pair.second;

    TRACE(BLD_PATTERN,
          8,
          "Replacing method with its original version %s",
          SHOW(method));
    method->set_code(copy->release_code());
    DexMethod::erase_method(copy);
    DexMethod::delete_method_DO_NOT_USE(copy);
  }
}

} // namespace builder_pattern
