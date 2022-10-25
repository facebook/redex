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
 */

namespace optimize_enums {
namespace {

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
    for (auto* insn_cmp : res.matching(cmp_location)) {
      auto cmp_it = m_cfg.find_insn(insn_cmp);
      always_assert(!cmp_it.is_end());

      // ...find the aget instr supplying it. From the aget we find the
      // field being used to do the lookup, as well as the instruction
      // that results in the ordinal.
      auto* insn_aget = res.matching(cmp_location, insn_cmp, aget_src).unique();

      auto* insn_look = res.matching(m_flow.aget, insn_aget, 0).unique();
      if (!insn_look) {
        continue;
      }

      auto* insn_ordi = res.matching(m_flow.aget, insn_aget, 1).unique();
      if (!insn_ordi) {
        continue;
      }

      // Grab the inverse lookup from case-value to enum. Our lookup
      // instruction was constrained to only match on fields present
      // in generated_switch_cases, so this entry always exists.
      const auto* lookup_field = insn_look->get_field();
      const auto case_to_enum_it = m_generated_switch_cases.find(lookup_field);

      always_assert(case_to_enum_it != m_generated_switch_cases.end());
      const auto& case_to_enum = case_to_enum_it->second;

      // Stash the ordinal in a new temporary register.
      const reg_t ordinal_reg = m_cfg.allocate_temp();
      copy_ordinal(insn_ordi, ordinal_reg);

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

  void copy_ordinal(IRInstruction* insn_ordi, reg_t ordinal_reg) {
    auto ordi_it = m_cfg.find_insn(insn_ordi);
    always_assert(!ordi_it.is_end());

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

    for (cfg::Edge* succ : succs) {
      switch_old_cases.emplace_back(succ->case_key());

      if (!succ->case_key()) {
        // This is the default case, it remains unchanged.
        continue;
      }

      const int32_t case_value = *succ->case_key();
      if (case_value == 0) {
        TRACE(ENUM, 1, "Unexpected zero case value: %s", SHOW(switch_insn));

        rollback();
        return;
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

      // Turn the case value back into the enum ordinal.
      const auto enum_ordinal_maybe =
          get_ordinal_for_case(case_to_enum, case_value);
      if (!enum_ordinal_maybe) {
        // We don't actually have a full mapping.
        return;
      }

      int32_t enum_ordinal = *enum_ordinal_maybe;

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

  // We allow multiple const sources for a comparison, hence the use
  // of forall instead of uniq. This doesn't occur often, but there
  // are scenarios where other passes leave such patterns.
  auto uniq = mf::alias | mf::unique;
  auto forall = mf::alias | mf::forall;

  kase = flow.insn(m::const_());
  lookup = flow.insn(m_lookup);
  ordinal = flow.insn(m_invoke_ordinal);
  aget = flow.insn(m::aget_()).src(0, lookup, uniq).src(1, ordinal, uniq);

  // See top of file for why IF_EQZ/IF_NEZ are omitted.
  cmp_switch = flow.insn(m::switch_()).src(0, aget, uniq);
  cmp_if_src0 = flow.insn(m::if_eq_() || m::if_ne_())
                    .src(0, aget, uniq)
                    .src(1, kase, forall);
  cmp_if_src1 = flow.insn(m::if_eq_() || m::if_ne_())
                    .src(0, kase, forall)
                    .src(1, aget, uniq);
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
