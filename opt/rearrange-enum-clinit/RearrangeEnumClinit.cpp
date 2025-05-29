/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RearrangeEnumClinit.h"

#include <atomic>
#include <optional>

#include "Debug.h"
#include "DeterministicContainers.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "LiveRange.h"
#include "MethodUtil.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace rearrange_enum_clinit {
namespace {

struct Rearranger {
  DexMethod* m;
  cfg::ControlFlowGraph& cfg;
  cfg::Block* b;

  live_range::MoveAwareChains mac;
  live_range::DefUseChains def_use;
  live_range::UseDefChains use_def;

  UnorderedMap<const IRInstruction*, IRList::iterator> insn_map;

  IRInstruction* array_sput{nullptr};
  IRList::iterator array_new_array;
  reg_t alloc_reg;

  Rearranger(DexMethod* m, cfg::ControlFlowGraph& cfg)
      : m(m),
        cfg(cfg),
        b(cfg.entry_block()),
        mac(cfg),
        def_use(mac.get_def_use_chains()),
        use_def(mac.get_use_def_chains()),
        insn_map([](auto* block) {
          UnorderedMap<const IRInstruction*, IRList::iterator> map;
          for (auto it = block->begin(); it != block->end(); ++it) {
            if (it->type == MFLOW_OPCODE) {
              map.emplace(it->insn, it);
            }
          }
          return map;
        }(cfg.entry_block())) {}

  IRInstruction* find_values_sput() {
    // Optimistically look for the `$VALUES` field and accept it.
    for (auto it = b->rbegin(); it != b->rend(); ++it) {
      if (it->type == MFLOW_OPCODE &&
          it->insn->opcode() == OPCODE_SPUT_OBJECT) {
        auto* f = it->insn->get_field();
        if (f->get_class() == m->get_class() &&
            f->get_name()->str() == "$VALUES") {
          return it->insn;
        }
      }
    }

    // Look for the `values()` function and analyze it.
    auto* c = type_class(m->get_class());
    redex_assert(c != nullptr);

    auto* values_method = [&c]() -> DexMethod* {
      for (auto* dm : c->get_dmethods()) {
        if (dm->get_name()->str() == "values") {
          auto* p = dm->get_proto();
          if (p->get_args()->empty() && type::is_array(p->get_rtype())) {
            return dm;
          }
        }
      }
      return nullptr;
    }();

    if (values_method == nullptr) {
      return nullptr;
    }

    auto* field = analyze_values_method(values_method);
    if (field == nullptr) {
      return nullptr;
    }

    for (auto it = b->rbegin(); it != b->rend(); ++it) {
      if (it->type == MFLOW_OPCODE &&
          it->insn->opcode() == OPCODE_SPUT_OBJECT) {
        if (field == it->insn->get_field()) {
          return it->insn;
        }
      }
    }

    return nullptr;
  }

  static DexFieldRef* analyze_values_method(DexMethod* values_method) {
    cfg::ScopedCFG cfg(values_method->get_code());

    std::optional<IRInstruction*> ret_opt{};
    for (auto& mie : cfg::InstructionIterable(*cfg)) {
      if (mie.insn->opcode() == OPCODE_RETURN_OBJECT) {
        if (ret_opt) {
          return nullptr; // Single return only.
        }
        ret_opt = mie.insn;
      }
    }
    redex_assert(ret_opt);

    using namespace live_range;
    MoveAwareChains mac(*cfg);
    auto use_def = mac.get_use_def_chains();

    auto get_singleton = [&use_def](IRInstruction* insn,
                                    src_index_t idx) -> IRInstruction* {
      auto it = use_def.find(Use{insn, idx});
      if (it == use_def.end()) {
        return nullptr;
      }
      if (it->second.size() != 1) {
        return nullptr;
      }
      return *it->second.begin();
    };

    for (IRInstruction* insn = *ret_opt; insn != nullptr;) {
      // Written this way to ensure safe coding, always make progress.
      src_index_t use_idx;
      switch (insn->opcode()) {
      case OPCODE_RETURN_OBJECT:
      case OPCODE_CHECK_CAST:
        use_idx = 0;
        break;

      case OPCODE_SGET_OBJECT: {
        auto* f = insn->get_field();
        if (f->get_class() == values_method->get_class() &&
            type::get_element_type_if_array(f->get_type()) == f->get_class()) {
          return f;
        }
        return nullptr;
      }

      case OPCODE_INVOKE_VIRTUAL: {
        auto* mref = insn->get_method();
        // Only support `clone()`.
        if (mref->get_name()->str() != "clone" ||
            mref->get_proto()->get_rtype() != type::java_lang_Object()) {
          return nullptr;
        }
        use_idx = 0;
      } break;

      // Unsupported opcodes.
      default:
        return nullptr;
      }

      insn = get_singleton(insn, use_idx);
    }

    return nullptr;
  }

  reg_t move_new_array_to_front() {
    redex_assert(array_new_array != CONSTP(b)->begin());

    auto size_def_it = use_def.find({array_new_array->insn, 0});
    redex_assert(size_def_it != std::as_const(use_def).end());
    redex_assert(size_def_it->second.size() == 1);
    auto orig_new_array_size_cfg_it =
        cfg.find_insn(*size_def_it->second.begin(), b);
    redex_assert(!orig_new_array_size_cfg_it.is_end());
    redex_assert(orig_new_array_size_cfg_it.block() == b);
    auto orig_new_array_size_it = orig_new_array_size_cfg_it.unwrap();
    always_assert(orig_new_array_size_it->insn->opcode() == OPCODE_CONST);

    // Just move to the front. This does not handle source blocks. Assume
    // this is not important for now.
    auto new_array_it = b->begin();
    while (new_array_it->type != MFLOW_OPCODE) {
      ++new_array_it;
    }

    // Should not be necessary, but for safety.
    reg_t size_reg = cfg.allocate_temp();

    reg_t new_reg = cfg.allocate_temp();

    b->insert_before(
        b->to_cfg_instruction_iterator(new_array_it),
        {
            (new IRInstruction(*orig_new_array_size_it->insn))
                ->set_dest(size_reg),
            (new IRInstruction(*array_new_array->insn))->set_src(0, size_reg),
            (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                ->set_dest(new_reg),
        });

    array_sput->set_src(0, new_reg);

    // Leave the size around in case it was referenced for something else.
    // Will be cleaned up by shrinker.

    return new_reg;
  }

  IRInstruction* find_singleton_def(IRInstruction* use_insn,
                                    src_index_t src_index) {
    auto it = use_def.find(live_range::Use{use_insn, src_index});
    redex_assert(it != use_def.cend());
    redex_assert(it->second.size() == 1);
    return *it->second.begin();
  }

  std::pair<IRList::iterator, reg_t> find_move_point_new_instance(
      IRInstruction* object_insn) {
    auto it = def_use.find(object_insn);
    redex_assert(it != def_use.cend());
    std::optional<reg_t> which_reg{};
    std::optional<IRList::iterator> which_it{};
    for (auto& obj_use : UnorderedIterable(it->second)) {
      if (obj_use.src_index == 0 &&
          opcode::is_an_invoke(obj_use.insn->opcode()) &&
          method::is_constructor(obj_use.insn->get_method())) {
        redex_assert(!which_reg);
        which_reg = obj_use.insn->src(0);
        which_it = insn_map.at(obj_use.insn);
      }
    }
    redex_assert(which_reg);
    return std::make_pair(*which_it, *which_reg);
  }

  std::pair<IRList::iterator, reg_t> find_move_point(
      IRInstruction* object_insn) {
    if (object_insn->opcode() != OPCODE_NEW_INSTANCE) {
      auto object_it = insn_map.at(object_insn);

      if (object_insn->has_move_result_any()) {
        ++object_it;
        while (object_it->type != MFLOW_OPCODE) {
          ++object_it;
        }
        redex_assert(opcode::is_move_result_any(object_it->insn->opcode()));
      }
      return std::make_pair(object_it, object_it->insn->dest());
    }
    return find_move_point_new_instance(object_insn);
  }

  bool run() {
    // Find a sput-object for `$VALUES`.
    array_sput = find_values_sput();
    if (array_sput == nullptr) {
      return false;
    }
    redex_assert(array_sput->opcode() == OPCODE_SPUT_OBJECT);

    // Find the definition of the field object.
    array_new_array = insn_map.at(find_singleton_def(array_sput, 0));

    if (array_new_array->insn->opcode() != OPCODE_NEW_ARRAY) {
      // Possibly Kotlin enum with `.$values()` not inlined, yet.
      return false;
    }

    bool moved_new_array{false};
    auto move_array = [&]() {
      if (moved_new_array) {
        return;
      }
      alloc_reg = move_new_array_to_front();
      moved_new_array = true;
    };

    // Move array to front, use a new register.

    // Find all the users of the array. This should be aput-object things.
    {
      auto new_array_uses_it = def_use.find(array_new_array->insn);
      redex_assert(new_array_uses_it != def_use.cend());
      std::optional<reg_t> extra_reg{};
      for (auto& use : UnorderedIterable(new_array_uses_it->second)) {
        // Skip the sput.
        if (use.insn == array_sput) {
          continue;
        }
        assert_log(use.insn->opcode() == OPCODE_APUT_OBJECT,
                   "Unexpected opcode %x",
                   use.insn->opcode());

        // Check what the definition is.
        IRInstruction* object_insn = find_singleton_def(use.insn, 0);
        if (object_insn->opcode() == OPCODE_SGET_OBJECT) {
          // Field read style, does not really benefit from moving.
          continue;
        }

        // OK, may be beneficial.
        move_array();

        if (!extra_reg) {
          extra_reg = cfg.allocate_temp();
        }

        // See if we can hoist it. Check the input parameters.
        // 1. First parameter is object. Already checked above.

        // 2. Third parameter is index, should be const.
        IRInstruction* index_insn = find_singleton_def(use.insn, 2);
        redex_assert(index_insn->opcode() == OPCODE_CONST);

        // We need to find the point where the object is fully
        // constructed. If the object_insn is a NEW_INSTANCE, search for the
        // following <init> call. Otherwise just use the result.
        auto [insert_after_it, obj_reg] = find_move_point(object_insn);

        b->insert_after(
            b->to_cfg_instruction_iterator(insert_after_it),
            {
                (new IRInstruction(*index_insn))->set_dest(*extra_reg),
                (new IRInstruction(*use.insn))
                    ->set_src(0, obj_reg)
                    ->set_src(1, alloc_reg)
                    ->set_src(2, *extra_reg),
            });

        // Remove the old aput.
        b->remove_insn(cfg.find_insn(use.insn, b).unwrap());
      }
    }

    if (!moved_new_array) {
      return false;
    }

    // Finally remove the old new-array. Do it late so there's no undefined
    // behavior with deleted things.
    b->remove_insn(array_new_array);

    return true;
  }
};

} // namespace

MethodResult RearrangeEnumClinitPass::run(DexMethod* m, IRCode* code) {
  auto cfg = cfg::ScopedCFG(code);
  if (cfg->num_blocks() != 1) {
    return MethodResult::kNotOneBlock;
  }

  auto res = Rearranger(m, *cfg).run();

  return res ? MethodResult::kChanged : MethodResult::kFailed;
}

void RearrangeEnumClinitPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  std::atomic<size_t> cnt_all{0};
  std::atomic<size_t> cnt_not_one_block{0};
  std::atomic<size_t> cnt_no_clinit{0};
  std::atomic<size_t> cnt_below_threshold{0};
  std::atomic<size_t> cnt_failed{0};
  std::atomic<size_t> cnt_changed{0};
  std::atomic<size_t> cnt_no_optimizations{0};

  walk::parallel::classes(build_class_scope(stores), [&](DexClass* c) {
    if (c->is_external() || !is_enum(c)) {
      return;
    }

    cnt_all.fetch_add(1, std::memory_order_relaxed);

    auto* m = c->get_clinit();
    if (m == nullptr) {
      // This case can happen for anonymous classes used when an enum case is
      // specialized.
      cnt_no_clinit.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (m->get_code()->count_opcodes() < m_threshold) {
      cnt_below_threshold.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (m->rstate.no_optimizations()) {
      cnt_no_optimizations.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    auto res = run(m, m->get_code());

    switch (res) {
    case MethodResult::kUnknown:
      not_reached();

    case MethodResult::kNotOneBlock:
      cnt_not_one_block.fetch_add(1, std::memory_order_relaxed);
      break;

    case MethodResult::kFailed:
      cnt_failed.fetch_add(1, std::memory_order_relaxed);
      break;

    case MethodResult::kChanged:
      cnt_changed.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  });

  mgr.set_metric("changed", cnt_changed.load());
  mgr.set_metric("failed", cnt_failed.load());
  mgr.set_metric("no_clinit", cnt_no_clinit.load());
  mgr.set_metric("below_threshold", cnt_below_threshold.load());
  mgr.set_metric("no_optimizations", cnt_no_optimizations.load());
  mgr.set_metric("not_one_block", cnt_not_one_block.load());
  mgr.set_metric("all_enum", cnt_all.load());
}

static RearrangeEnumClinitPass s_pass;

} // namespace rearrange_enum_clinit
