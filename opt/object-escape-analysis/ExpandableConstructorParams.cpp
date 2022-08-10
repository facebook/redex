/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ExpandableConstructorParams.h"
#include "ApiLevelChecker.h"
#include "CFGMutation.h"
#include "LiveRange.h"
#include "Resolver.h"
#include "Show.h"
#include "StringBuilder.h"
#include "Walkers.h"

namespace {
// Predict what a method's deobfuscated name would be.
std::string show_deobfuscated(const DexType* type,
                              const DexString* name,
                              const DexProto* proto) {
  string_builders::StaticStringBuilder<5> b;
  b << show_deobfuscated(type) << "." << show(name) << ":"
    << show_deobfuscated(proto);
  return b.str();
}
} // namespace

std::vector<DexType*> ExpandableConstructorParams::get_expanded_args_vector(
    DexMethod* ctor,
    param_index_t param_index,
    const std::vector<DexField*>& fields) {
  always_assert(param_index > 0);
  auto args = ctor->get_proto()->get_args();
  always_assert(param_index <= args->size());
  std::vector<DexType*> args_vector;
  args_vector.reserve(args->size() - 1 + fields.size());
  for (param_index_t i = 0; i < args->size(); i++) {
    if (i != param_index - 1) {
      args_vector.push_back(args->at(i));
      continue;
    }
    for (auto f : fields) {
      args_vector.push_back(f->get_type());
    }
  }
  return args_vector;
}

// Get or create the class-info for a given type.
ExpandableConstructorParams::ClassInfo*
ExpandableConstructorParams::get_class_info(DexType* type) const {
  auto res = m_class_infos.get(type, nullptr);
  if (res) {
    return res.get();
  }
  res = std::make_shared<ClassInfo>();
  std::set<std::vector<DexType*>> args_vectors;
  auto cls = type_class(type);
  if (cls) {
    // First, collect all of the (guaranteed to be distinct) args of the
    // existing constructors.
    for (auto* ctor : cls->get_ctors()) {
      auto args = ctor->get_proto()->get_args();
      std::vector<DexType*> args_vector(args->begin(), args->end());
      auto inserted = args_vectors.insert(std::move(args_vector)).second;
      always_assert(inserted);
    }
    // Second, for each ctor, and each (non-first) parameter that is only used
    // in igets, compute the expanded constructor args and record them if they
    // don't create a conflict.
    for (auto* ctor : cls->get_ctors()) {
      auto code = ctor->get_code();
      if (!code || ctor->rstate.no_optimizations()) {
        continue;
      }
      live_range::MoveAwareChains chains(code->cfg());
      auto du_chains = chains.get_def_use_chains();
      param_index_t param_index{1};
      auto ii = code->cfg().get_param_instructions();
      for (auto it = std::next(ii.begin()); it != ii.end();
           it++, param_index++) {
        bool expandable{true};
        std::vector<DexField*> fields;
        for (auto& use : du_chains[it->insn]) {
          if (opcode::is_an_iget(use.insn->opcode())) {
            auto* field =
                resolve_field(use.insn->get_field(), FieldSearch::Instance);
            if (field) {
              fields.push_back(field);
              continue;
            }
          }
          expandable = false;
          break;
        }
        if (!expandable) {
          continue;
        }
        std::sort(fields.begin(), fields.end(), compare_dexfields);
        // remove duplicates
        fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
        auto expanded_args_vector =
            get_expanded_args_vector(ctor, param_index, fields);
        // We need to check if we don't have too many args that won't fit into
        // an invoke/range instruction.
        uint32_t range_size = 1;
        for (auto arg_type : expanded_args_vector) {
          range_size += type::is_wide_type(arg_type) ? 2 : 1;
        }
        if (range_size <= 0xff) {
          auto inserted =
              args_vectors.insert(std::move(expanded_args_vector)).second;
          if (inserted) {
            (*res)[ctor].emplace(param_index, std::move(fields));
          }
        }
      }
    }
  }
  m_class_infos.update(type, [&](auto*, auto& value, bool exists) {
    if (exists) {
      // Oh well, we wasted some racing with another thread.
      res = value;
      return;
    }
    value = res;
  });
  return res.get();
}

// Given an earlier created expanded constructor method ref, fill in the code.
DexMethod* ExpandableConstructorParams::make_expanded_ctor_concrete(
    DexMethodRef* expanded_ctor_ref) {
  auto [ctor, param_index] = m_candidates.at(expanded_ctor_ref);

  // We start from the original ctor method body, and mutate a copy.
  std::unique_ptr<IRCode> cloned_code =
      std::make_unique<IRCode>(std::make_unique<cfg::ControlFlowGraph>());
  ctor->get_code()->cfg().deep_copy(&cloned_code->cfg());
  auto& cfg = cloned_code->cfg();
  cfg::CFGMutation mutation(cfg);

  // Replace load-param of (newly created) object with a sequence of
  // load-params for the field values used by the ctor; initialize the (newly
  // created) object register with a const-0, so that any remaining
  // move-object instructions are still valid.
  auto block = cfg.entry_block();
  auto load_param_it =
      block->to_cfg_instruction_iterator(block->get_first_insn());
  always_assert(!load_param_it.is_end());
  for (param_index_t i = 0; i < param_index; i++) {
    load_param_it++;
    always_assert(!load_param_it.is_end());
  }
  auto last_load_params_it =
      block->to_cfg_instruction_iterator(block->get_last_param_loading_insn());
  auto null_insn = (new IRInstruction(OPCODE_CONST))
                       ->set_dest(load_param_it->insn->dest())
                       ->set_literal(0);
  mutation.insert_after(last_load_params_it, {null_insn});

  std::vector<IRInstruction*> new_load_param_insns;
  std::unordered_map<DexField*, reg_t> field_regs;
  auto& fields =
      m_class_infos.at_unsafe(ctor->get_class())->at(ctor).at(param_index);
  for (auto field : fields) {
    auto reg = type::is_wide_type(field->get_type()) ? cfg.allocate_wide_temp()
                                                     : cfg.allocate_temp();
    auto inserted = field_regs.emplace(field, reg).second;
    always_assert(inserted);
    auto load_param_insn =
        (new IRInstruction(opcode::load_opcode(field->get_type())))
            ->set_dest(reg);
    new_load_param_insns.push_back(load_param_insn);
  }
  mutation.replace(load_param_it, new_load_param_insns);

  // Replace all igets on the (newly created) object with moves from the new
  // field value load-params. No other (non-move) uses of the (newly created)
  // object can exist.
  live_range::MoveAwareChains chains(cfg);
  auto du_chains = chains.get_def_use_chains();
  std::unordered_set<IRInstruction*> use_insns;
  for (auto& use : du_chains[load_param_it->insn]) {
    use_insns.insert(use.insn);
  }
  auto ii = InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); it++) {
    if (!use_insns.count(it->insn)) {
      continue;
    }
    auto insn = it->insn;
    always_assert(opcode::is_an_iget(insn->opcode()));
    auto* field = resolve_field(insn->get_field(), FieldSearch::Instance);
    always_assert(field);
    auto move_result_pseudo_it = cfg.move_result_of(it);
    always_assert(!move_result_pseudo_it.is_end());
    auto reg = field_regs.at(field);
    auto dest = move_result_pseudo_it->insn->dest();
    auto move_insn = (new IRInstruction(opcode::move_opcode(field->get_type())))
                         ->set_src(0, reg)
                         ->set_dest(dest);
    mutation.replace(it, {move_insn});
  }

  // Use the mutated copied ctor code to concretize the expanded ctor.
  mutation.flush();
  expanded_ctor_ref->make_concrete(ACC_CONSTRUCTOR | ACC_PUBLIC,
                                   std::move(cloned_code), false);
  auto expanded_ctor = expanded_ctor_ref->as_def();
  always_assert(expanded_ctor);
  expanded_ctor->rstate.set_generated();
  int api_level = api::LevelChecker::get_method_level(ctor);
  expanded_ctor->rstate.set_api_level(api_level);
  expanded_ctor->set_deobfuscated_name(show_deobfuscated(expanded_ctor));
  return expanded_ctor;
}

ExpandableConstructorParams::ExpandableConstructorParams(const Scope& scope) {
  walk::classes(scope, [&](DexClass* cls) {
    for (auto ctor : cls->get_ctors()) {
      auto deob = ctor->get_deobfuscated_name_or_null();
      if (deob) {
        m_deobfuscated_ctor_names.insert(deob);
      }
    }
  });
}

// Try to create a method-ref that represents an expanded ctor, where a
// particular parameter representing a (newly created) object gets replaced by
// a sequence of field values used by the ctor.
DexMethodRef* ExpandableConstructorParams::get_expanded_ctor_ref(
    DexMethod* ctor,
    param_index_t param_index,
    std::vector<DexField*>** fields) const {
  auto type = ctor->get_class();
  auto class_info = get_class_info(type);
  auto it = class_info->find(ctor);
  if (it == class_info->end()) {
    return nullptr;
  }
  auto it2 = it->second.find(param_index);
  if (it2 == it->second.end()) {
    return nullptr;
  }

  auto name = ctor->get_name();
  auto args_vector = get_expanded_args_vector(ctor, param_index, it2->second);
  auto type_list = DexTypeList::make_type_list(std::move(args_vector));
  auto proto = DexProto::make_proto(type::_void(), type_list);

  auto deob = show_deobfuscated(type, name, proto);
  if (m_deobfuscated_ctor_names.count(DexString::make_string(deob))) {
    // Some other method ref already has the synthetic deobfuscated name that
    // we'd later want to give to the new generated ctor.
    return nullptr;
  }

  std::lock_guard<std::mutex> lock_guard(m_candidates_mutex);
  auto expanded_ctor_ref = DexMethod::get_method(type, name, proto);
  if (expanded_ctor_ref) {
    if (!m_candidates.count(expanded_ctor_ref)) {
      // There's already a pre-existing method registered, maybe a method that
      // became unreachable. As other Redex optimizations might have persisted
      // this method-ref, we don't want to interact with it.
      return nullptr;
    }
  } else {
    expanded_ctor_ref = DexMethod::make_method(type, name, proto);
    always_assert(show_deobfuscated(expanded_ctor_ref) == deob);
    auto emplaced =
        m_candidates
            .emplace(expanded_ctor_ref, std::make_pair(ctor, param_index))
            .second;
    always_assert(emplaced);
  }
  *fields = &it2->second;
  return expanded_ctor_ref;
}

// Make sure that all newly used expanded ctors actually exist as concrete
// methods.
size_t ExpandableConstructorParams::flush(const Scope& scope) {
  // First, find all expanded_ctor_ref that made it into the updated code.
  ConcurrentSet<DexMethodRef*> used_expanded_ctor_refs;
  walk::parallel::opcodes(scope, [&](DexMethod*, IRInstruction* insn) {
    if (opcode::is_invoke_direct(insn->opcode()) &&
        m_candidates.count(insn->get_method())) {
      used_expanded_ctor_refs.insert(insn->get_method());
    }
  });

  // Second, make them all concrete.
  ConcurrentSet<DexMethod*> expanded_ctors;
  workqueue_run<DexMethodRef*>(
      [&](DexMethodRef* expanded_ctor_ref) {
        expanded_ctors.insert(make_expanded_ctor_concrete(expanded_ctor_ref));
      },
      used_expanded_ctor_refs);

  // Add the newly concretized ctors to their classes.
  std::vector<DexMethod*> ordered(expanded_ctors.begin(), expanded_ctors.end());
  std::sort(ordered.begin(), ordered.end(), compare_dexmethods);
  for (auto expanded_ctor : ordered) {
    type_class(expanded_ctor->get_class())->add_method(expanded_ctor);
  }

  // Finally, erase the unused ctor method refs.
  for (auto [ctor, param_index] : m_candidates) {
    if (!used_expanded_ctor_refs.count(ctor)) {
      DexMethod::erase_method(ctor);
      DexMethod::delete_method_DO_NOT_USE(static_cast<DexMethod*>(ctor));
    }
  }

  return expanded_ctors.size();
}
