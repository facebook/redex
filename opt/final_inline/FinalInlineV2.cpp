/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "FinalInlineV2.h"

#include <boost/variant.hpp>
#include <unordered_set>
#include <vector>

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "ConstantPropagationWholeProgramState.h"
#include "Debug.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IPConstantPropagationAnalysis.h"
#include "IRCode.h"
#include "LocalDce.h"
#include "Resolver.h"
#include "Walkers.h"

/*
 * dx-generated class initializers often use verbose bytecode sequences to
 * initialize static fields, instead of relying on the more compact
 * encoded_value formats. This pass determines the values of the static
 * fields after the <clinit> has finished running, which it uses to generate
 * their encoded_value equivalents. This applies to both final statics and
 * non-final statics.
 *
 * Additionally, for static final fields, this pass inlines sgets to them where
 * possible, replacing them with const / const-wide / const-string opcodes.
 */

namespace cp = constant_propagation;

namespace {

class StringAnalyzer
    : public InstructionAnalyzerBase<StringAnalyzer, ConstantEnvironment> {
 public:
  static bool analyze_const_string(const IRInstruction* insn,
                                   ConstantEnvironment* env) {
    env->set(RESULT_REGISTER, StringDomain(insn->get_string()));
    return true;
  }
};

/*
 * Foo.<clinit> may read some static fields from class Bar, in which case
 * Bar.<clinit> will be executed first by the VM to determine the values of
 * those fields.
 *
 * Similarly, to ensure that our analysis of Foo.<clinit> knows as much about
 * Bar's static fields as possible, we want to analyze Bar.<clinit> before
 * Foo.<clinit>, since Foo.<clinit> depends on it. As such, we do a topological
 * sort of the classes here based on these dependencies.
 *
 * Note that the class initialization graph is *not* guaranteed to be acyclic.
 * (JLS SE7 12.4.1 indicates that cycles are indeed allowed.) In that case,
 * this pass cannot safely optimize the static final constants.
 */
Scope reverse_tsort_by_clinit_deps(const Scope& scope) {
  Scope result;
  std::unordered_set<const DexClass*> visiting;
  std::unordered_set<const DexClass*> visited;
  std::function<void(DexClass*)> visit = [&](DexClass* cls) {
    if (visited.count(cls)) {
      return;
    }
    if (visiting.count(cls)) {
      throw class_initialization_cycle(cls);
    }
    visiting.emplace(cls);
    auto clinit = cls->get_clinit();
    if (clinit != nullptr && clinit->get_code() != nullptr) {
      for (auto& mie : InstructionIterable(clinit->get_code())) {
        auto insn = mie.insn;
        if (is_sget(insn->opcode())) {
          auto dependee_cls = type_class(insn->get_field()->get_class());
          if (dependee_cls == nullptr || dependee_cls == cls) {
            continue;
          }
          visit(dependee_cls);
        }
      }
    }
    visiting.erase(cls);
    result.emplace_back(cls);
    visited.emplace(cls);
  };
  for (DexClass* cls : scope) {
    visit(cls);
  }
  return result;
}

using CombinedAnalyzer =
    InstructionAnalyzerCombiner<cp::ClinitFieldAnalyzer,
                                cp::WholeProgramAwareAnalyzer,
                                StringAnalyzer,
                                cp::PrimitiveAnalyzer>;

// A trivial clinit should only contain a return-void instruction.
bool is_trivial_clinit(const DexMethod* method) {
  auto ii = InstructionIterable(method->get_code());
  return std::none_of(ii.begin(), ii.end(), [](const MethodItemEntry& mie) {
    return mie.insn->opcode() != OPCODE_RETURN_VOID;
  });
}

/*
 * Converts a ConstantValue into its equivalent encoded_value. Returns null if
 * no such encoding is known.
 */
class encoding_visitor : public boost::static_visitor<DexEncodedValue*> {
 public:
  encoding_visitor(const DexField* field) : m_field(field) {}

  DexEncodedValue* operator()(const SignedConstantDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return nullptr;
    }
    auto ev = DexEncodedValue::zero_for_type(m_field->get_type());
    ev->value(static_cast<uint64_t>(*cst));
    return ev;
  }

  DexEncodedValue* operator()(const StringDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return nullptr;
    }
    return new DexEncodedValueString(const_cast<DexString*>(*cst));
  }

  template <typename Domain>
  DexEncodedValue* operator()(const Domain&) const {
    return nullptr;
  }

 private:
  const DexField* m_field;
};

void encode_values(DexClass* cls, StaticFieldEnvironment field_env) {
  for (auto* field : cls->get_sfields()) {
    auto value = field_env.get(field);
    auto encoded_value =
        ConstantValue::apply_visitor(encoding_visitor(field), value);
    if (encoded_value == nullptr) {
      continue;
    }
    field->make_concrete(field->get_access(), encoded_value);
    TRACE(FINALINLINE,
          2,
          "Found encodable field: %s %s\n",
          SHOW(field),
          SHOW(value));
  }
}

/*
 * This method determines the values of the static fields after the <clinit>
 * has finished running and generates their encoded_value equivalents.
 *
 * Additionally, for static final fields, this method collects and returns them
 * as part of the WholeProgramState object.
 */
cp::WholeProgramState analyze_and_simplify_clinits(const Scope& scope) {
  cp::WholeProgramState wps;
  for (DexClass* cls : reverse_tsort_by_clinit_deps(scope)) {
    ConstantEnvironment env;
    cp::set_encoded_values(cls, &env);
    auto clinit = cls->get_clinit();
    if (clinit != nullptr && clinit->get_code() != nullptr) {
      auto* code = clinit->get_code();
      code->build_cfg();
      auto& cfg = code->cfg();
      cfg.calculate_exit_block();
      cp::intraprocedural::FixpointIterator intra_cp(
          cfg, CombinedAnalyzer(cls->get_type(), &wps, nullptr, nullptr));
      intra_cp.run(env);
      env = intra_cp.get_exit_state_at(cfg.exit_block());

      // Generate the encoded_values and re-run the analysis.
      encode_values(cls, env.get_field_environment());
      auto fresh_env = ConstantEnvironment();
      cp::set_encoded_values(cls, &fresh_env);
      intra_cp.run(fresh_env);

      // Detect any field writes made redundant by the new encoded_values and
      // remove those sputs.
      cp::Transform::Config transform_config;
      transform_config.class_under_init = cls->get_type();
      cp::Transform(transform_config).apply(intra_cp, wps, code);
      // Delete the instructions rendered dead by the removal of those sputs.
      LocalDcePass::run(clinit);
      // If the clinit is empty now, delete it.
      if (is_trivial_clinit(clinit)) {
        cls->remove_method(clinit);
      }
    }
    wps.collect_static_finals(cls, env.get_field_environment());
  }
  return wps;
}

size_t inline_static_final_gets(const Scope& scope,
                                const cp::WholeProgramState& wps) {
  size_t inlined_count{0};
  walk::code(scope, [&](const DexMethod* method, IRCode& code) {
    if (is_clinit(method)) {
      return;
    }
    std::vector<std::pair<IRInstruction*, std::vector<IRInstruction*>>>
        replacements;
    for (auto& mie : InstructionIterable(code)) {
      auto it = code.iterator_to(mie);
      auto insn = mie.insn;
      auto op = insn->opcode();
      if (is_sget(op)) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Static);
        auto replacement = ConstantValue::apply_visitor(
            cp::value_to_instruction_visitor(
                ir_list::move_result_pseudo_of(it)),
            wps.get_field_value(field));
        if (replacement.size() == 0) {
          continue;
        }
        replacements.emplace_back(insn, replacement);
      }
    }
    for (auto const& p : replacements) {
      code.replace_opcode(p.first, p.second);
    }
    inlined_count += replacements.size();
  });
  return inlined_count;
}

// XXX(jezng): In principle, we should avoid deleting a field if
// can_delete(field) is false, even if can_delete(containing class) is true.
// However, the previous implementation of FinalInline did this more aggressive
// deletion -- deleting as long as either can_delete(field) or can_delete(cls)
// is true -- and I'm following it so as not to cause a regression. The right
// long-term fix is to clean up the proguard keep rules -- then we can just rely
// on RMU to delete these fields.
void aggressively_delete_static_finals(const Scope& scope) {
  std::unordered_set<const DexField*> referenced_fields;
  walk::opcodes(scope, [&](const DexMethod*, const IRInstruction* insn) {
    if (!insn->has_field()) {
      return;
    }
    auto field = resolve_field(insn->get_field(), FieldSearch::Static);
    referenced_fields.emplace(field);
  });
  for (auto* cls : scope) {
    auto& sfields = cls->get_sfields();
    sfields.erase(
        std::remove_if(sfields.begin(),
                       sfields.end(),
                       [&](DexField* field) {
                         return is_final(field) && !field->is_external() &&
                                referenced_fields.count(field) == 0 &&
                                (can_delete(cls) || can_delete(field));
                       }),
        sfields.end());
  }
}

} // namespace

size_t FinalInlinePassV2::run(const Scope& scope) {
  try {
    auto wps = analyze_and_simplify_clinits(scope);
    return inline_static_final_gets(scope, wps);
  } catch (class_initialization_cycle& e) {
    std::cerr << e.what();
    return 0;
  }
}

void FinalInlinePassV2::run_pass(DexStoresVector& stores,
                                 ConfigFiles& cfg,
                                 PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(FINALINLINE,
          1,
          "FinalInlinePassV2 not run because no ProGuard configuration was "
          "provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  auto inlined_count = run(scope);

  if (m_aggressively_delete) {
    aggressively_delete_static_finals(scope);
  }

  mgr.incr_metric("num_finals_inlined", inlined_count);
}

static FinalInlinePassV2 s_pass;
