/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnumsUnmap.h"

#include "CFGMutation.h"
#include "Show.h"
#include "Trace.h"

/**
 * A switch map is initialized with the *runtime* length of the enum values()
 * array. The known enums are then assigned a strictly positive case value,
 * which is put in the switch map; i.e. `lookup[enum.ordinal()] = case;`.
 *
 * It is possible that the runtime enum has more variants than the clinit
 * knew about when compiled. Because the switch map used the runtime length,
 * it has a safety net: an unknown ordinal maps to zero. When an enum is
 * switched over, it actually switches over `lookup[enum.ordinal()]`. All of
 * the switch cases are converted to the known positive case values and the
 * default case is used to handle unknown ordinals (i.e., zero).
 *
 * ---
 *
 * When a switch is converted to a sequence of if statements by D8, the
 * default case becomes the trailing `else` case. Although a switch map
 * lookup could result in 0, it is not explicitly listed as a case.
 *
 * Due to this we can omit looking at IF_EQZ/IF_NEZ opcodes. Even if we
 * came across an explicit 0 case, unmapping that would require non-trivial
 * rearrangement to turn it back into the `else` case. Given that this
 * doesn't occur in practice, implementing this is omitted; if we see a
 * case value of 0, we give up as the fallback.
 *
 * -----
 *
 * In Kotlin, a switchmap may not be the only source value to a switch. When
 * switching over a nullable enum, the null case is "switch mapped" to -1.
 * This allows the null case to gracefully remain alongside the others:
 *
 *     val foo: Foo? = ...;
 *     when (foo) {
 *         null -> <null case>
 *         Foo.THIS -> <this case>
 *         Foo.THAT -> <that case>
 *     }
 *
 * Compiles into something analogous to:
 *
 *     val mapped = if (foo == null) {
 *         -1
 *     } else {
 *         T$WhenMappings.$EnumSwitchMapping[foo.ordinal()]
 *     };
 *     when (mapped) {
 *         -1 -> <null case>
 *          1 -> <this case>
 *          2 -> <that case>
 *     }
 *
 * This causes problems for unmapping, because there is no enum corresponding
 * to -1 in a switchmap. However, because -1 is not a valid ordinal we can
 * simply consider the "ordinal" of a null enum to be -1 as well. This allows
 * us to leave the case values of -1 untouched:
 *
 *     val unmapped = if (foo == null) {
 *         -1
 *     } else {
 *         foo.ordinal()
 *     };
 *     when (unmapped) {
 *         -1 -> <null case>
 *          0 -> <this case>
 *          1 -> <that case>
 *     }
 *
 * We are effectively pretending the const -1 instruction is ordinal().
 *
 * ---
 *
 * A quirk of the above Kotlin null handling is that in order to use a
 * packed switch, the 0 case (which is typically left as the default case)
 * must be made explicit. This is counter to the vanilla Java invariants
 * described above.
 *
 * When D8 turns such a switch into a sequence of if statements it merges
 * the 0 case with the default case. As a result, there is no explicit if
 * statement comparing to zero and our typical invariants hold.
 *
 * However, when the switch remains as such then we may see an "unexpected"
 * case of 0. As long as it goes to the same target as the default case,
 * we will consider it obsolete once we unmap and delete it. (The case 0
 * slot will be filled by an ordinal instead.)
 */

namespace optimize_enums {
namespace {

// The ordinal we will give a null Kotlin enum.
//
// This is able to be the same value as the null Kotlin enum switchmap value,
// because non-null ordinals are always >= 0. In turn, this allows us to do
// minimal changes to the structure of switches and keep them packed.
constexpr int32_t kKotlinNullOrdinal = -1;

/**
 * The state and helper functions while unmapping a CFG.
 *
 * For every matching cmp instr, we apply the inverse mapping.
 *
 *  INVOKE_VIRTUAL <v_enum> <Enum>;.ordinal:()
 *  MOVE_RESULT <v_ordinal>
 *  ...
 *  AGET <v_switchmap>, <v_ordinal>
 *  MOVE_RESULT_PSEUDO <v_mapped>
 *  ...
 *  IF_EQ <v_mapped> C  // Some constant C.
 *
 * Becomes:
 *
 *  INVOKE_VIRTUAL <v_enum> <Enum>;.ordinal:()
 *  MOVE_RESULT <v_ordinal>
 *  MOVE <v_ordinal_cpy> <v_ordinal> // Newly added
 *  ...
 *  AGET <v_switchmap>, <v_ordinal>  // Dead code
 *  MOVE_RESULT_PSEUDO <v_mapped>  // Dead code
 *  ...
 *  IF_EQ <v_ordinal_cpy> C'
 *
 * Where C' is the ordinal that would switchmap to C.
 *
 * This causes use of the switch map to become dead code. We rely
 * on a DCE pass to actually remove it all once it has become unused.
 */
class OptimizeEnumsUnmapCfg {
 public:
  OptimizeEnumsUnmapCfg(const OptimizeEnumsUnmapMatchFlow& flow,
                        const EnumFieldToOrdinal& enum_field_to_ordinal,
                        const GeneratedSwitchCases& generated_switch_cases,
                        cfg::ControlFlowGraph& cfg)
      : m_flow(flow),
        m_enum_field_to_ordinal(enum_field_to_ordinal),
        m_generated_switch_cases(generated_switch_cases),
        m_cfg(cfg),
        m_mutation(cfg) {}

  void unmap_switchmaps() {
    auto cmp_locations = {
        m_flow.cmp_switch,
        m_flow.cmp_if_src0,
        m_flow.cmp_if_src1,
    };
    auto res = m_flow.flow.find(m_cfg, cmp_locations);

    for (auto cmp_location : cmp_locations) {
      unmap_location(res, cmp_location);
    }

    m_mutation.flush();
  }

 private:
  void unmap_location(const mf::result_t& res, mf::location_t cmp_location) {
    // The source index the switchmap lookup flows into.
    //
    // Src 0 for switch and if_src0, and src 1 for if_scr1.
    const src_index_t aget_src = cmp_location == m_flow.cmp_if_src1;

    // For each matching comparison...
    for (auto* insn_cmp : res.order(res.matching(cmp_location))) {
      auto cmp_it = m_cfg.find_insn(insn_cmp);
      always_assert(!cmp_it.is_end());

      // ...find the aget instrs supplying it. From each aget we find the
      // field being used to do the lookup, and can continue as long as
      // all of them use the identical switchmap. While we're doing this
      // we also gather up all the places an ordinal feeds into the map.
      auto insn_aget_or_m1_range =
          res.matching(cmp_location, insn_cmp, aget_src);
      const auto [lookup_field, insn_ordinal_list] =
          get_lookup_and_ordinals(res, insn_aget_or_m1_range);

      if (!lookup_field) {
        // No clear switchmap to undo. Unactionable in general.
        continue;
      }

      // Grab the inverse lookup from case-value to enum. Our lookup
      // instruction was constrained to only match on fields present
      // in generated_switch_cases, so this entry always exists.
      const auto case_to_enum_it = m_generated_switch_cases.find(lookup_field);

      always_assert(case_to_enum_it != m_generated_switch_cases.end());
      const auto& case_to_enum = case_to_enum_it->second;

      // Stash the ordinal sources in a new temporary register.
      const reg_t ordinal_reg = m_cfg.allocate_temp();
      for (IRInstruction* insn_ordi : insn_ordinal_list) {
        copy_ordinal(insn_ordi, ordinal_reg);
      }

      // Finally, update the comparison.
      if (cmp_location == m_flow.cmp_switch) {
        unmap_switch(insn_cmp, cmp_it, case_to_enum, ordinal_reg);
      } else {
        always_assert(cmp_location == m_flow.cmp_if_src0 ||
                      cmp_location == m_flow.cmp_if_src1);

        // Grab all the incoming const instructions.
        const src_index_t const_src = 1 - aget_src;

        const auto insn_kase_range =
            res.matching(cmp_location, insn_cmp, const_src);
        always_assert(insn_kase_range);

        unmap_if(insn_cmp, cmp_it, case_to_enum, ordinal_reg, aget_src,
                 const_src, insn_kase_range);
      }
    }
  }

  std::tuple<DexFieldRef*, std::vector<IRInstruction*>> get_lookup_and_ordinals(
      const mf::result_t& res, mf::result_t::src_range insn_aget_or_m1_range) {
    DexFieldRef* unique_lookup_field = nullptr;
    std::vector<IRInstruction*> insn_ordinal_list;

    for (IRInstruction* insn_aget_or_m1 : insn_aget_or_m1_range) {
      // Kotlin null enum "ordinal"; see top of file.
      if (opcode::is_const(insn_aget_or_m1->opcode())) {
        always_assert(insn_aget_or_m1->get_literal() == kKotlinNullOrdinal);

        insn_ordinal_list.push_back(insn_aget_or_m1);
        continue;
      }

      // Every lookup field must be *identical* to safely unmap.
      for (IRInstruction* insn_look :
           res.matching(m_flow.aget_or_m1, insn_aget_or_m1, 0)) {
        auto* lookup_field = insn_look->get_field();

        if (unique_lookup_field == nullptr) {
          unique_lookup_field = lookup_field;
        } else if (lookup_field != unique_lookup_field) {
          TRACE(ENUM, 1, "Mismatched switchmap lookup fields; %s is not %s",
                SHOW(lookup_field), SHOW(unique_lookup_field));

          return {nullptr, {}};
        }
      }

      // Remember where all the ordinal results are, for later copying.
      for (IRInstruction* insn_ordi :
           res.matching(m_flow.aget_or_m1, insn_aget_or_m1, 1)) {
        insn_ordinal_list.push_back(insn_ordi);
      }
    }

    always_assert(!unique_lookup_field || !insn_ordinal_list.empty());
    return std::make_tuple(unique_lookup_field, std::move(insn_ordinal_list));
  }

  void copy_ordinal(IRInstruction* insn_ordi, reg_t ordinal_reg) {
    auto ordi_it = m_cfg.find_insn(insn_ordi);
    always_assert(!ordi_it.is_end());

    // Kotlin null enum ordinal; see top of file.
    if (opcode::is_const(insn_ordi->opcode())) {
      always_assert(insn_ordi->get_literal() == kKotlinNullOrdinal);

      IRInstruction* ordinal_const = new IRInstruction(OPCODE_CONST);
      ordinal_const->set_dest(ordinal_reg);
      ordinal_const->set_literal(kKotlinNullOrdinal);

      m_mutation.insert_after(ordi_it, {ordinal_const});
      return;
    }

    auto ordi_move_result_it = m_cfg.move_result_of(ordi_it);
    always_assert(!ordi_move_result_it.is_end());

    const auto reg_ordinal = ordi_move_result_it->insn->dest();

    const auto move_ordinal_result = new IRInstruction(OPCODE_MOVE);
    move_ordinal_result->set_src(0, reg_ordinal);
    move_ordinal_result->set_dest(ordinal_reg);

    m_mutation.insert_after(ordi_move_result_it, {move_ordinal_result});
  }

  boost::optional<size_t> get_ordinal_for_case(
      const GeneratedSwitchCasetoField& case_to_enum, int64_t case_value) {
    // Turn the case value back into the enum ordinal.
    const auto case_enum_it = case_to_enum.find(case_value);
    if (case_enum_it == case_to_enum.end()) {
      // We don't actually have a full inverse mapping.
      return boost::none;
    }
    const auto case_enum = case_enum_it->second;

    const auto enum_ordinal_it = m_enum_field_to_ordinal.find(case_enum);
    if (enum_ordinal_it == m_enum_field_to_ordinal.end()) {
      // We don't actually have a full ordinal mapping.
      return boost::none;
    }
    const auto enum_ordinal = enum_ordinal_it->second;

    return enum_ordinal;
  }

  void unmap_switch(IRInstruction* switch_insn,
                    const cfg::InstructionIterator& switch_it,
                    const GeneratedSwitchCasetoField& case_to_enum,
                    reg_t ordinal_reg) {
    const auto* block = switch_it.block();
    const auto& succs = block->succs();

    // When unmapping a switch, we may rarely find partway through that
    // we don't have the inverse of every case. This is used to store an
    // undo stack on the off-chance we hit this case.
    std::vector<cfg::Edge::MaybeCaseKey> switch_old_cases;
    switch_old_cases.reserve(succs.size());

    const auto rollback = [&] {
      for (size_t undo = 0; undo != switch_old_cases.size(); ++undo) {
        succs[undo]->set_case_key(switch_old_cases[undo]);
      }
    };

    // When a Kotlin switch handles null (case -1), then case 0 will be
    // present to keep the switch packed. It goes to the same block as
    // the default case. Once we unmap, this case edge can be deleted
    // because an ordinal of 0 will take its place.
    cfg::Edge* default_edge = nullptr;
    cfg::Edge* obsolete_zero_edge = nullptr;

    for (cfg::Edge* succ : succs) {
      switch_old_cases.emplace_back(succ->case_key());

      if (!succ->case_key()) {
        // This is the default case, it remains unchanged.
        default_edge = succ;
        continue;
      }

      const int32_t case_value = *succ->case_key();
      if (case_value == 0) {
        // Kotlin null enum default case; see top of file.
        obsolete_zero_edge = succ;
        continue;
      } else if (case_value == kKotlinNullOrdinal) {
        // Kotlin null enum ordinal; see top of file.
        continue;
      }

      // Turn the case value back into the enum ordinal.
      const auto enum_ordinal_maybe =
          get_ordinal_for_case(case_to_enum, case_value);
      if (!enum_ordinal_maybe) {
        // We don't actually have a full mapping. Undo any
        // modifications we've made so far to roll back.
        rollback();
        return;
      }

      const size_t enum_ordinal = *enum_ordinal_maybe;
      succ->set_case_key(enum_ordinal);
    }

    if (obsolete_zero_edge) {
      // This edge is now overwritten with ordinal 0, and not needed.
      if (default_edge &&
          obsolete_zero_edge->target() == default_edge->target()) {
        m_cfg.delete_edge(obsolete_zero_edge);
      } else {
        TRACE(ENUM, 1, "Unexpected zero case value: %s", SHOW(switch_insn));

        rollback();
        return;
      }
    }

    // Success. The source is now the copied ordinal.
    switch_insn->set_src(0, ordinal_reg);
  }

  void unmap_if(IRInstruction* if_cmp_insn,
                const cfg::InstructionIterator& if_cmp_it,
                const GeneratedSwitchCasetoField& case_to_enum,
                reg_t ordinal_reg,
                reg_t aget_src,
                reg_t const_src,
                mf::result_t::src_range insn_kase_range) {
    // We allocate a new const register, and after each of the constants
    // in the range we insert a new unmapped const. (Updating the const
    // in place would be an error, since it could be used elsewhere.)
    //
    // If at any point we decide to roll back these changes, we can just
    // return. The new register and const instructions will be dead.
    reg_t new_const_reg = m_cfg.allocate_temp();

    for (IRInstruction* insn_kase : insn_kase_range) {
      const int64_t case_value = insn_kase->get_literal();
      if (case_value == 0) {
        TRACE(ENUM, 1, "Unexpected zero if case value: %s", SHOW(if_cmp_insn));

        return;
      }

      int32_t enum_ordinal;
      if (case_value == kKotlinNullOrdinal) {
        // Kotlin null enum ordinal; see top of file.
        enum_ordinal = kKotlinNullOrdinal;
      } else {
        // Turn the case value back into the enum ordinal.
        const auto enum_ordinal_maybe =
            get_ordinal_for_case(case_to_enum, case_value);
        if (!enum_ordinal_maybe) {
          // We don't actually have a full mapping.
          return;
        }

        enum_ordinal = *enum_ordinal_maybe;
      }

      auto kase_it = m_cfg.find_insn(insn_kase);
      always_assert(!kase_it.is_end());

      IRInstruction* ordinal_const = new IRInstruction(OPCODE_CONST);
      ordinal_const->set_dest(new_const_reg);
      ordinal_const->set_literal(enum_ordinal);

      m_mutation.insert_after(kase_it, {ordinal_const});
    }

    // Success. Update the if to compare with the ordinal.
    if_cmp_insn->set_src(aget_src, ordinal_reg);
    if_cmp_insn->set_src(const_src, new_const_reg);
  }

  const OptimizeEnumsUnmapMatchFlow& m_flow;
  const EnumFieldToOrdinal& m_enum_field_to_ordinal;
  const GeneratedSwitchCases& m_generated_switch_cases;

  cfg::ControlFlowGraph& m_cfg;
  cfg::CFGMutation m_mutation;
};

} // namespace

OptimizeEnumsUnmapMatchFlow::OptimizeEnumsUnmapMatchFlow(
    const GeneratedSwitchCases& generated_switch_cases) {
  // The flow is: an ordinal into any switchmap lookup that we have
  // the mapping for, which then goes to a comparison with a constant.
  DexMethod* java_enum_ordinal =
      resolve_method(DexMethod::get_method("Ljava/lang/Enum;.ordinal:()I"),
                     MethodSearch::Virtual);
  always_assert(java_enum_ordinal);

  auto m_invoke_ordinal = m::invoke_virtual_(m::has_method(
      m::resolve_method(MethodSearch::Virtual, m::equals(java_enum_ordinal))));
  auto m_lookup = m::sget_object_(
      m::has_field(m::in<DexFieldRef*>(generated_switch_cases)));
  auto m_minus1 =
      m::const_(m::has_literal(m::equals<int64_t>(kKotlinNullOrdinal)));

  // We allow multiple sources to the aget as well as multiple consts for
  // a comparison, hence the use of forall instead of unique. This doesn't
  // occur often, but there are cases where other passes leave such patterns.
  //
  // For nullable Kotlin enums, we must handle that possibility that the
  // switch source is either an aget switchmap or -1 for nulls.
  auto forall = mf::alias | mf::forall;

  kase = flow.insn(m::const_());
  lookup = flow.insn(m_lookup);
  ordinal = flow.insn(m_invoke_ordinal);
  aget_or_m1 = flow.insn(m::aget_() || m_minus1)
                   .src(0, lookup, forall)
                   .src(1, ordinal, forall);

  // See top of file for why IF_EQZ/IF_NEZ are omitted.
  cmp_switch = flow.insn(m::switch_()).src(0, aget_or_m1, forall);
  cmp_if_src0 = flow.insn(m::if_eq_() || m::if_ne_())
                    .src(0, aget_or_m1, forall)
                    .src(1, kase, forall);
  cmp_if_src1 = flow.insn(m::if_eq_() || m::if_ne_())
                    .src(0, kase, forall)
                    .src(1, aget_or_m1, forall);
}

OptimizeEnumsUnmap::OptimizeEnumsUnmap(
    const EnumFieldToOrdinal& enum_field_to_ordinal,
    const GeneratedSwitchCases& generated_switch_cases)
    : m_flow(generated_switch_cases),
      m_enum_field_to_ordinal(enum_field_to_ordinal),
      m_generated_switch_cases(generated_switch_cases) {}

void OptimizeEnumsUnmap::unmap_switchmaps(cfg::ControlFlowGraph& cfg) const {
  OptimizeEnumsUnmapCfg unmap_cfg(m_flow, m_enum_field_to_ordinal,
                                  m_generated_switch_cases, cfg);
  unmap_cfg.unmap_switchmaps();
}

} // namespace optimize_enums
