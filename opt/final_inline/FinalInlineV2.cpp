/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FinalInlineV2.h"

#include <boost/variant.hpp>
#include <unordered_set>
#include <vector>

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
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
  std::unordered_set<const DexClass*> scope_set(scope.begin(), scope.end());
  Scope result;
  std::unordered_set<const DexClass*> visiting;
  std::unordered_set<const DexClass*> visited;
  std::function<void(DexClass*)> visit = [&](DexClass* cls) {
    if (visited.count(cls) != 0 || scope_set.count(cls) == 0) {
      return;
    }
    if (visiting.count(cls)) {
      throw final_inline::class_initialization_cycle(cls);
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

/**
 * Similar to reverse_tsort_by_clinit_deps(...), but since we are currently
 * only dealing with instance field from class that only have one <init>
 * so stop when we are at a class that don't have exactly one constructor,
 * we are not dealing with them now so we won't have knowledge about their
 * instance field.
 */
Scope reverse_tsort_by_init_deps(const Scope& scope) {
  std::unordered_set<const DexClass*> scope_set(scope.begin(), scope.end());
  Scope result;
  std::unordered_set<const DexClass*> visiting;
  std::unordered_set<const DexClass*> visited;
  std::function<void(DexClass*)> visit = [&](DexClass* cls) {
    if (visited.count(cls) != 0 || scope_set.count(cls) == 0) {
      return;
    }
    visiting.emplace(cls);
    const auto& ctors = cls->get_ctors();
    if (ctors.size() == 1) {
      auto ctor = ctors[0];
      if (ctor != nullptr && ctor->get_code() != nullptr) {
        for (auto& mie : InstructionIterable(ctor->get_code())) {
          auto insn = mie.insn;
          if (is_iget(insn->opcode())) {
            auto dependee_cls = type_class(insn->get_field()->get_class());
            if (dependee_cls == nullptr || dependee_cls == cls) {
              continue;
            }
            visit(dependee_cls);
          }
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
                                cp::StringAnalyzer,
                                cp::PrimitiveAnalyzer>;

using CombinedInitAnalyzer =
    InstructionAnalyzerCombiner<cp::InitFieldAnalyzer,
                                cp::WholeProgramAwareAnalyzer,
                                cp::StringAnalyzer,
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

void encode_values(DexClass* cls, FieldEnvironment field_env) {
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

} // namespace

namespace final_inline {

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
      code->build_cfg(/* editable */ false);
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
      LocalDcePass::run(code);
      // If the clinit is empty now, delete it.
      if (is_trivial_clinit(clinit)) {
        cls->remove_method(clinit);
      }
    }
    wps.collect_static_finals(cls, env.get_field_environment());
  }
  return wps;
}

/*
 * Similar to analyze_and_simplify_clinits().
 * This method determines the values of the instance fields after the <init>
 * has finished running and generates their encoded_value equivalents.
 *
 * Unlike static field, if instance field were changed outside of <init>, the
 * instance field might have different value for different class instance. And
 * for class with multiple <init>, the outcome of ifields might be different
 * based on which constructor was used when initializing the instance. So
 * we are only considering class with only one <init>.
 */
cp::WholeProgramState analyze_and_simplify_inits(
    const Scope& scope, const cp::EligibleIfields& eligible_ifields) {
  cp::WholeProgramState wps;
  for (DexClass* cls : reverse_tsort_by_init_deps(scope)) {
    if (cls->is_external()) {
      continue;
    }
    ConstantEnvironment env;
    auto ctors = cls->get_ctors();
    if (ctors.size() > 1) {
      continue;
    }
    cp::set_ifield_values(cls, eligible_ifields, &env);
    if (ctors.size() == 1) {
      auto ctor = ctors[0];
      if (ctor->get_code() != nullptr) {
        auto* code = ctor->get_code();
        code->build_cfg(/* editable */ false);
        auto& cfg = code->cfg();
        cfg.calculate_exit_block();
        cp::intraprocedural::FixpointIterator intra_cp(
            cfg, CombinedInitAnalyzer(cls->get_type(), &wps, nullptr, nullptr));
        intra_cp.run(env);
        env = intra_cp.get_exit_state_at(cfg.exit_block());

        // Remove redundant iputs in inits
        cp::Transform::Config transform_config;
        transform_config.class_under_init = cls->get_type();
        cp::Transform(transform_config).apply(intra_cp, wps, code);
        // Delete the instructions rendered dead by the removal of those iputs.
        LocalDcePass::run(code);
      }
    }
    wps.collect_instance_finals(
        cls, eligible_ifields, env.get_field_environment());
  }
  return wps;
}

} // namespace final_inline

namespace {

/**
 * This function adds instance fields in cls_to_check that the method
 * accessed in blacklist_ifields.
 */
void get_ifields_read(const DexType* ifield_cls,
                      const DexMethod* method,
                      std::unordered_set<DexField*>* blacklist_ifields) {
  if (method == nullptr || method->get_code() == nullptr) {
    return;
  }
  for (auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (is_iget(insn->opcode())) {
      auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
      if (field != nullptr && field->get_class() == ifield_cls) {
        blacklist_ifields->emplace(field);
      }
    }
  }
}

/**
 * This function add ifields like x in following example in blacklist to avoid
 * inlining them.
 *   class Foo {
 *     final int x;
 *     Foo() {
 *       bar();
 *       x = 1;
 *     }
 *     bar() {
 *       // x is zero here, we don't want FinalInline to make it take value 1.
 *       if (x == 1) { ... }
 *     }
 *   }
 */
std::unordered_set<DexField*> get_ifields_read_in_callees(const Scope& scope) {
  std::unordered_set<DexField*> return_ifields;
  walk::classes(scope, [&](DexClass* cls) {
    if (cls->is_external()) {
      return;
    }
    auto ctors = cls->get_ctors();
    if (ctors.size() != 1) {
      // We are not inlining ifields in multi-ctors class so can also ignore
      // them here.
      return;
    }
    auto ctor = ctors[0];
    if (ctor->get_code() != nullptr) {
      auto* code = ctor->get_code();
      for (auto& mie : InstructionIterable(code)) {
        auto insn = mie.insn;
        if (is_invoke(insn->opcode())) {
          auto callee = resolve_method(insn->get_method(), MethodSearch::Any);
          get_ifields_read(cls->get_type(), callee, &return_ifields);
        }
      }
    }
  });
  return return_ifields;
}

cp::EligibleIfields gather_ifield_candidates(const Scope& scope) {
  cp::EligibleIfields eligible_ifields;
  std::unordered_set<DexField*> ifields_candidates;
  walk::fields(scope, [&](DexField* field) {
    // Collect non-final instance field candidates that are non external,
    // and can be deleted.
    if (is_static(field) || field->is_external() || !can_delete(field) ||
        is_volatile(field)) {
      return;
    }
    if (is_final(field)) {
      eligible_ifields.emplace(field);
      return;
    }
    DexClass* field_cls = type_class(field->get_class());
    if (field_cls != nullptr && field_cls->get_ctors().size() > 1) {
      // Class with multiple constructors, ignore it now.
      return;
    }
    ifields_candidates.emplace(field);
  });

  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    // Remove candidate field if it was written in code other than its class'
    // init function.
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      auto op = insn->opcode();
      if (is_iput(op)) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
        if (field == nullptr ||
            (is_init(method) && method->get_class() == field->get_class())) {
          // If couldn't resolve the field, or this method is this field's
          // class's init function, move on.
          continue;
        }
        // We assert that final fields are not modified outside of <init>
        // methods. javac seems to enforce this, but it's unclear if the JVM
        // spec actually forbids that. Doing the check here simplifies the
        // constant propagation analysis later -- we can determine the values
        // of these fields without analyzing any methods invoked from the
        // <init> methods.
        always_assert_log(!is_final(field),
                          "FinalInlinePassV2: encountered one final instance "
                          "field been changed outside of its class's <init> "
                          "file, for temporary solution set "
                          "\"inline_instance_field\" in \"FinalInlinePassV2\" "
                          "to be false.");
        ifields_candidates.erase(field);
      }
    }
  });
  for (DexField* field : ifields_candidates) {
    eligible_ifields.emplace(field);
  }
  std::unordered_set<DexField*> blacklist_ifields =
      get_ifields_read_in_callees(scope);
  for (DexField* field : blacklist_ifields) {
    eligible_ifields.erase(field);
  }
  return eligible_ifields;
}

size_t inline_final_gets(
    const Scope& scope,
    const cp::WholeProgramState& wps,
    const std::unordered_set<const DexType*>& black_list_types,
    cp::FieldType field_type) {
  size_t inlined_count{0};
  walk::code(scope, [&](const DexMethod* method, IRCode& code) {
    if (field_type == cp::FieldType::STATIC && is_clinit(method)) {
      return;
    }
    std::vector<std::pair<IRInstruction*, std::vector<IRInstruction*>>>
        replacements;
    for (auto& mie : InstructionIterable(code)) {
      auto it = code.iterator_to(mie);
      auto insn = mie.insn;
      auto op = insn->opcode();
      if (is_iget(op) || is_sget(op)) {
        auto field = resolve_field(insn->get_field());
        if (field == nullptr || black_list_types.count(field->get_class())) {
          continue;
        }
        if (field_type == cp::FieldType::INSTANCE && is_init(method) &&
            method->get_class() == field->get_class()) {
          // Don't propagate a field's value in ctors of its class with value
          // after ctor finished.
          continue;
        }
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

size_t FinalInlinePassV2::run(const Scope& scope, Config config) {
  try {
    auto wps = final_inline::analyze_and_simplify_clinits(scope);
    return inline_final_gets(
        scope, wps, config.black_list_types, cp::FieldType::STATIC);
  } catch (final_inline::class_initialization_cycle& e) {
    std::cerr << e.what();
    return 0;
  }
}

size_t FinalInlinePassV2::run_inline_ifields(
    const Scope& scope,
    const cp::EligibleIfields& eligible_ifields,
    Config config) {
  auto wps = final_inline::analyze_and_simplify_inits(scope, eligible_ifields);
  return inline_final_gets(
      scope, wps, config.black_list_types, cp::FieldType::INSTANCE);
}

void FinalInlinePassV2::run_pass(DexStoresVector& stores,
                                 ConfigFiles& /* conf */,
                                 PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(FINALINLINE,
          1,
          "FinalInlinePassV2 not run because no ProGuard configuration was "
          "provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  auto inlined_sfields_count = run(scope, m_config);
  size_t inlined_ifields_count{0};
  if (m_config.inline_instance_field) {
    cp::EligibleIfields eligible_ifields = gather_ifield_candidates(scope);
    inlined_ifields_count =
        run_inline_ifields(scope, eligible_ifields, m_config);
  }
  if (m_config.aggressively_delete) {
    aggressively_delete_static_finals(scope);
  }

  mgr.incr_metric("num_static_finals_inlined", inlined_sfields_count);
  mgr.incr_metric("num_instance_finals_inlined", inlined_ifields_count);
}

static FinalInlinePassV2 s_pass;
