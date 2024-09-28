/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WrappedPrimitives.h"

#include <inttypes.h>
#include <mutex>

#include "CFGMutation.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationState.h"
#include "ConstantPropagationWholeProgramState.h"
#include "ConstructorParams.h"
#include "DexUtil.h"
#include "IPConstantPropagationAnalysis.h"
#include "InitDeps.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "Show.h"
#include "Trace.h"
#include "TypeSystem.h"
#include "Walkers.h"
#include "WorkQueue.h"

namespace wrapped_primitives {
static std::unique_ptr<WrappedPrimitives> s_instance{nullptr};
WrappedPrimitives* get_instance() { return s_instance.get(); }

void initialize(const std::vector<Spec>& wrapper_specs) {
  s_instance.reset(new WrappedPrimitives(wrapper_specs));
  // In tests, we create and destroy g_redex repeatedly. So we need to reset
  // the singleton.
  g_redex->add_destruction_task([]() { s_instance.reset(nullptr); });
}

void WrappedPrimitives::mark_roots() {
  for (auto& spec : m_wrapper_specs) {
    for (auto&& [from, to] : spec.allowed_invokes) {
      auto def = to->as_def();
      if (def != nullptr && def->rstate.can_delete()) {
        TRACE(WP, 2, "Setting %s as root", SHOW(def));
        def->rstate.set_root();
        m_marked_root_methods.emplace(def);
        auto cls = type_class(def->get_class());
        if (cls->rstate.can_delete()) {
          TRACE(WP, 2, "Setting %s as root", SHOW(cls));
          cls->rstate.set_root();
          m_marked_root_classes.emplace(cls);
        }
      }
    }
    for (auto& method : spec.wrapper_type_constructors()) {
      if (!method->rstate.dont_inline()) {
        method->rstate.set_dont_inline();
        TRACE(WP, 2, "Disallowing inlining for %s", SHOW(method));
      }
    }
  }
}

void WrappedPrimitives::unmark_roots() {
  for (auto& def : m_marked_root_methods) {
    TRACE(WP, 2, "Unsetting %s as root", SHOW(def));
    def->rstate.unset_root();
  }
  for (auto& cls : m_marked_root_classes) {
    TRACE(WP, 2, "Unsetting %s as root", SHOW(cls));
    cls->rstate.unset_root();
  }
}

namespace {
bool contains_relevant_invoke(
    const std::unordered_set<const DexMethodRef*>& wrapped_apis,
    DexMethod* method) {
  if (wrapped_apis.empty()) {
    return false;
  }
  auto& cfg = method->get_code()->cfg();
  auto iterable = cfg::InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    IRInstruction* insn = it->insn;
    if (insn->has_method() && wrapped_apis.count(insn->get_method()) > 0) {
      return true;
    }
  }
  return false;
}

// Checks if the value is a known ObjectWithImmutAttr with a single known
// attribute value. Makes assumptions that there is only 1, as is consistent
// with the other assumptions in the pass.
boost::optional<std::pair<const DexType*, int64_t>>
extract_object_with_attr_value(const ConstantValue& value) {
  auto obj_or_none = value.maybe_get<ObjectWithImmutAttrDomain>();
  if (obj_or_none != boost::none &&
      obj_or_none->get_constant() != boost::none) {
    auto object = *obj_or_none->get_constant();
    always_assert(object.attributes.size() == 1);
    auto signed_value =
        object.attributes.front().value.maybe_get<SignedConstantDomain>();
    if (signed_value != boost::none &&
        signed_value.value().get_constant() != boost::none) {
      auto primitive_value = *signed_value.value().get_constant();
      return std::pair<const DexType*, int64_t>(object.type, primitive_value);
    } else {
      TRACE(WP, 2, "  No SignedConstantDomain value");
    }
  } else {
    TRACE(WP, 2, "  Not a known ObjectWithImmutAttrDomain");
  }

  return boost::none;
}

bool needs_cast(const TypeSystem& type_system,
                DexMethodRef* from_ref,
                DexMethodRef* to_ref) {
  auto from = from_ref->get_class();
  auto to = to_ref->get_class();
  if (from == to) {
    return false;
  }
  if (is_interface(type_class(from))) {
    auto supers = type_system.get_all_super_interfaces(from);
    return supers.count(to) == 0;
  } else {
    if (is_interface(type_class(to))) {
      return !type_system.implements(from, to);
    } else {
      return !type_system.is_subtype(to, from);
    }
  }
}
} // namespace

bool WrappedPrimitives::is_wrapped_method_ref(const DexType* wrapper_type,
                                              DexMethodRef* ref) {
  auto search = m_type_to_spec.find(const_cast<DexType*>(wrapper_type));
  if (search != m_type_to_spec.end()) {
    return search->second.allowed_invokes.count(ref) > 0;
  }
  return false;
};

DexMethodRef* WrappedPrimitives::unwrapped_method_ref_for(
    const DexType* wrapper_type, DexMethodRef* ref) {
  auto spec = m_type_to_spec.at(const_cast<DexType*>(wrapper_type));
  return spec.allowed_invokes.at(ref);
};

void WrappedPrimitives::increment_consts() { m_consts_inserted++; }

void WrappedPrimitives::increment_casts() { m_casts_inserted++; }

void WrappedPrimitives::optimize_method(
    const TypeSystem& type_system,
    const cp::intraprocedural::FixpointIterator& intra_cp,
    const cp::WholeProgramState& wps,
    DexMethod* method,
    cfg::ControlFlowGraph& cfg) {
  if (method->get_code() == nullptr || method->rstate.no_optimizations()) {
    return;
  }
  if (!contains_relevant_invoke(m_all_wrapped_apis, method)) {
    return;
  }

  TRACE(WP, 2, "optimize_method: %s", SHOW(method));
  cfg::CFGMutation mutation(cfg);
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable
    if (env.is_bottom()) {
      continue;
    }

    auto last_insn = block->get_last_insn();
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto cfg_it = block->to_cfg_instruction_iterator(it);
      auto move_result_it = cfg.move_result_of(cfg_it);

      auto insn = cfg_it->insn;
      if (insn->has_method() &&
          m_all_wrapped_apis.count(insn->get_method()) > 0) {
        TRACE(WP, 2, "Relevant invoke: %s in B%zu", SHOW(insn), block->id());
        auto ref = insn->get_method();

        // Inline the wrapped constant value and change method ref, if all
        // information is known.
        auto srcs_size = insn->srcs_size();
        auto& reg_env = env.get_register_environment();

        bool changed_ref{false};
        auto updated_insn = new IRInstruction(*insn);
        // HELPER FUNCTIONS
        auto perform_unwrapping = [&](const DexType* wrapper_type,
                                      src_index_t idx, reg_t literal_reg) {
          updated_insn->set_src(idx, literal_reg);
          auto unwrapped_ref = unwrapped_method_ref_for(wrapper_type, ref);
          if (!changed_ref) {
            if (needs_cast(type_system, ref, unwrapped_ref)) {
              auto to_type = unwrapped_ref->get_class();
              auto opcode = is_interface(type_class(to_type))
                                ? OPCODE_INVOKE_INTERFACE
                                : OPCODE_INVOKE_VIRTUAL;
              auto obj_reg = cfg.allocate_temp();
              auto cast = (new IRInstruction(OPCODE_CHECK_CAST))
                              ->set_type(to_type)
                              ->set_src(0, insn->src(0));
              auto move_pseudo =
                  (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                      ->set_dest(obj_reg);
              updated_insn->set_method(unwrapped_ref);
              updated_insn->set_opcode(opcode);
              updated_insn->set_src(0, obj_reg);
              mutation.insert_before(cfg_it, {cast, move_pseudo});
              increment_casts();
            } else {
              updated_insn->set_method(unwrapped_ref);
            }
            changed_ref = true;
          }
        };
        auto inject_const = [&](const cfg::InstructionIterator& anchor,
                                int64_t literal, reg_t literal_reg,
                                bool is_wide) {
          auto const_insn =
              (new IRInstruction(is_wide ? OPCODE_CONST_WIDE : OPCODE_CONST))
                  ->set_literal(literal)
                  ->set_dest(literal_reg);

          mutation.insert_before(anchor, {const_insn});
          increment_consts();
        };

        for (size_t i = 0; i < srcs_size; i++) {
          auto current_reg = insn->src(i);
          TRACE(WP, 2, "  Checking v%d", current_reg);
          auto& value = reg_env.get(current_reg);
          auto maybe_pair = extract_object_with_attr_value(value);
          if (maybe_pair != boost::none) {
            auto wrapper_type = maybe_pair->first;
            auto literal = maybe_pair->second;
            TRACE(WP,
                  2,
                  " ** Instruction %s uses a known object with constant "
                  "value %" PRId64,
                  SHOW(insn),
                  literal);

            auto search =
                m_type_to_spec.find(const_cast<DexType*>(wrapper_type));
            if (search != m_type_to_spec.end()) {
              auto spec = search->second;
              if (is_wrapped_method_ref(wrapper_type, ref)) {
                auto is_wide = type::is_wide_type(spec.primitive);
                auto literal_reg =
                    is_wide ? cfg.allocate_wide_temp() : cfg.allocate_temp();
                inject_const(cfg_it, literal, literal_reg, is_wide);
                perform_unwrapping(wrapper_type, i, literal_reg);
              }
            }
          }
        }
        // END FOR
        if (changed_ref) {
          std::vector<IRInstruction*> replacements{updated_insn};
          if (!move_result_it.is_end()) {
            replacements.push_back(new IRInstruction(*move_result_it->insn));
          }
          mutation.replace(cfg_it, replacements);
        }
      }
      intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
    }
  }
  mutation.flush();
}

bool is_wrapped_api(const DexMethodRef* ref) {
  auto wp_instance = get_instance();
  if (wp_instance == nullptr) {
    return false;
  }
  return wp_instance->is_wrapped_api(ref);
}

void optimize_method(const TypeSystem& type_system,
                     const cp::intraprocedural::FixpointIterator& intra_cp,
                     const cp::WholeProgramState& wps,
                     DexMethod* method,
                     cfg::ControlFlowGraph& cfg) {
  auto wp_instance = get_instance();
  if (wp_instance == nullptr) {
    return;
  }
  wp_instance->optimize_method(type_system, intra_cp, wps, method, cfg);
}
} // namespace wrapped_primitives
