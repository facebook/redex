/*
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
#include "Purity.h"
#include "Resolver.h"
#include "TypeSystem.h"
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
    if (visiting.count(cls) != 0) {
      TRACE(FINALINLINE, 1, "Possible class init cycle (could be benign):");
      for (auto visiting_cls : visiting) {
        TRACE(FINALINLINE, 1, "  %s", SHOW(visiting_cls));
      }
      TRACE(FINALINLINE, 1, "  %s", SHOW(cls));
      fprintf(stderr,
              "WARNING: Possible class init cycle found in FinalInlineV2. "
              "To check re-run with TRACE=FINALINLINE:1.\n");
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

/*
 * If a field is both read and written to in its initializer, then we can
 * update its encoded value with the value at exit only if the reads (sgets) are
 * are dominated by the writes (sputs) -- otherwise we may change program
 * semantics. Checking for dominance takes some work, and static fields are
 * rarely read in their class' <clinit>, so we simply avoid inlining all fields
 * that are read in their class' <clinit>.
 *
 * TODO: We should really transitively analyze all callees for field reads.
 * Right now this just analyzes the sgets directly in the <clinit>.
 */
std::unordered_set<const DexFieldRef*> gather_read_static_fields(
    DexClass* cls) {
  std::unordered_set<const DexFieldRef*> read_fields;
  auto clinit = cls->get_clinit();
  for (auto* block : clinit->get_code()->cfg().blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (is_sget(insn->opcode())) {
        read_fields.emplace(insn->get_field());
        TRACE(FINALINLINE, 3, "Found static field read in clinit: %s",
              SHOW(insn->get_field()));
      }
    }
  }
  return read_fields;
}

void encode_values(DexClass* cls,
                   const FieldEnvironment& field_env,
                   const std::unordered_set<const DexFieldRef*>& blacklist) {
  for (auto* field : cls->get_sfields()) {
    if (blacklist.count(field)) {
      continue;
    }
    auto value = field_env.get(field);
    auto encoded_value =
        ConstantValue::apply_visitor(encoding_visitor(field), value);
    if (encoded_value == nullptr) {
      continue;
    }
    field->set_value(encoded_value);
    TRACE(FINALINLINE, 2, "Found encodable field: %s %s", SHOW(field),
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
  const std::unordered_set<DexMethodRef*> pure_methods = get_pure_methods();
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

      // Generate the new encoded_values and re-run the analysis.
      encode_values(cls, env.get_field_environment(),
                    gather_read_static_fields(cls));
      auto fresh_env = ConstantEnvironment();
      cp::set_encoded_values(cls, &fresh_env);
      intra_cp.run(fresh_env);

      // Detect any field writes made redundant by the new encoded_values and
      // remove those sputs.
      cp::Transform::Config transform_config;
      transform_config.class_under_init = cls->get_type();
      cp::Transform(transform_config).apply(intra_cp, wps, code);
      // Delete the instructions rendered dead by the removal of those sputs.
      LocalDce(pure_methods).dce(code);
      // If the clinit is empty now, delete it.
      if (method::is_trivial_clinit(clinit)) {
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
  const std::unordered_set<DexMethodRef*> pure_methods = get_pure_methods();
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
        LocalDce(pure_methods).dce(code);
      }
    }
    wps.collect_instance_finals(cls, eligible_ifields,
                                env.get_field_environment());
  }
  return wps;
}

} // namespace final_inline

namespace {

namespace check_this {
using ThisDomain = sparta::ConstantAbstractDomain<bool>;
using ThisEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<uint32_t, ThisDomain>;

/**
 * Fixpoint analysis to track register that may hold "this" object, so that we
 * can use this info to find methods that are invoked on "this" object.
 * TODO(suree404): Switch to use existed LocalPointerAnalysis.
 */
class ThisObjectAnalysis final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               ThisEnvironment> {
 public:
  explicit ThisObjectAnalysis(cfg::ControlFlowGraph* cfg,
                              DexMethod* method,
                              size_t this_param_reg)
      : MonotonicFixpointIterator(*cfg, cfg->blocks().size()),
        m_method(method),
        m_this_param_reg(this_param_reg) {}
  void analyze_node(const NodeId& node, ThisEnvironment* env) const override {
    for (const auto& mie : InstructionIterable(*node)) {
      analyze_instruction(mie.insn, env);
    }
  }
  ThisEnvironment analyze_edge(
      cfg::Edge* const&,
      const ThisEnvironment& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  boost::optional<std::unordered_set<DexMethod*>>
  collect_method_called_on_this() {
    std::unordered_set<DexMethod*> return_set;
    auto* code = m_method->get_code();
    auto& cfg = code->cfg();
    for (cfg::Block* block : cfg.blocks()) {
      auto env = get_entry_state_at(block);

      auto ii = InstructionIterable(block);
      for (auto it = ii.begin(); it != ii.end(); it++) {
        IRInstruction* insn = it->insn;
        auto op = insn->opcode();
        if (is_invoke(op)) {
          bool use_this = false;
          for (auto src : insn->srcs()) {
            auto this_info = env.get(src).get_constant();
            if (!this_info || *this_info) {
              use_this = true;
              break;
            }
          }
          if (use_this) {
            auto insn_method = insn->get_method();
            auto callee = resolve_method(insn_method, opcode_to_search(insn));
            if (insn->opcode() == OPCODE_INVOKE_STATIC ||
                insn->opcode() == OPCODE_INVOKE_DIRECT) {
              if (callee != nullptr && callee->get_code() != nullptr) {
                return_set.emplace(callee);
              }
            } else {
              return_set.emplace(callee);
            }
          }
        } else if (op == OPCODE_IPUT_OBJECT || op == OPCODE_SPUT_OBJECT ||
                   op == OPCODE_APUT_OBJECT) {
          auto this_info = env.get(insn->src(0)).get_constant();
          if (!this_info || *this_info) {
            return boost::none;
          }
        } else if (op == OPCODE_FILLED_NEW_ARRAY) {
          for (auto src : insn->srcs()) {
            auto this_info = env.get(src).get_constant();
            if (!this_info || *this_info) {
              return boost::none;
            }
          }
        }
        analyze_instruction(insn, &env);
      }
    }
    return return_set;
  }

 private:
  void analyze_instruction(IRInstruction* insn, ThisEnvironment* env) const {
    auto default_case = [&]() {
      if (insn->has_dest()) {
        env->set(insn->dest(), ThisDomain(false));
      } else if (insn->has_move_result_any()) {
        env->set(RESULT_REGISTER, ThisDomain(false));
      }
    };
    switch (insn->opcode()) {
    case OPCODE_MOVE_OBJECT: {
      env->set(insn->dest(), env->get(insn->src(0)));
      break;
    }
    case IOPCODE_LOAD_PARAM_OBJECT: {
      if (insn->dest() == m_this_param_reg) {
        env->set(insn->dest(), ThisDomain(true));
      } else {
        env->set(insn->dest(), ThisDomain(false));
      }
      break;
    }
    case OPCODE_CHECK_CAST: {
      env->set(RESULT_REGISTER, env->get(insn->src(0)));
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
      env->set(insn->dest(), env->get(RESULT_REGISTER));
      break;
    }
    default: {
      default_case();
      break;
    }
    }
  }
  const DexMethod* m_method;
  size_t m_this_param_reg;
};
} // namespace check_this

/**
 * This function adds instance fields in cls_to_check that the method
 * accessed in blacklist_ifields.
 * Return false if all ifields are blacklisted - no need to check further.
 */
bool get_ifields_read(
    const std::unordered_set<std::string>& whitelist_method_names,
    const std::unordered_set<const DexType*>& parent_intf_set,
    const DexClass* ifield_cls,
    const DexMethod* method,
    ConcurrentSet<DexField*>* blacklist_ifields,
    std::unordered_set<const DexMethod*>* visited) {
  if (visited->count(method)) {
    return true;
  }
  visited->emplace(method);
  if (method != nullptr) {
    if (method::is_init(method) && parent_intf_set.count(method->get_class())) {
      // For call on its parent's ctor, no need to proceed.
      return true;
    }
    for (const auto& name : whitelist_method_names) {
      // Whitelisted methods name from config, ignore.
      // We have this whitelist so that we can ignore some methods that
      // are safe and won't read instance field.
      // TODO: Switch to a proper interprocedural fixpoint analysis.
      if (method->get_name()->str() == name) {
        return true;
      }
    }
  }
  if (method == nullptr || method->get_code() == nullptr) {
    // We can't track down further, don't process any ifields from ifield_cls.
    for (const auto& field : ifield_cls->get_ifields()) {
      blacklist_ifields->emplace(field);
    }
    return false;
  }
  for (auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (is_iget(insn->opcode())) {
      // Meet accessing of a ifield in a method called from <init>, add
      // to blacklist.
      auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
      if (field != nullptr && field->get_class() == ifield_cls->get_type()) {
        blacklist_ifields->emplace(field);
      }
    } else if (is_invoke(insn->opcode())) {
      auto insn_method = insn->get_method();
      auto callee = resolve_method(insn_method, opcode_to_search(insn));
      if (insn->opcode() == OPCODE_INVOKE_DIRECT ||
          insn->opcode() == OPCODE_INVOKE_STATIC) {
        // For invoke on a direct/static method, if we can't resolve them or
        // there is no code after resolved, those must be methods not
        // not implemented by us, so they won't access our instance fields
        // as well.
        if (!callee || !callee->get_code()) {
          continue;
        }
      } else {
        bool no_current_type = true;
        // No need to check on methods whose class/argumetns are not superclass
        // or interface of ifield_cls.
        if (callee != nullptr && !parent_intf_set.count(callee->get_class())) {
          for (const auto& type :
               callee->get_proto()->get_args()->get_type_list()) {
            if (parent_intf_set.count(type)) {
              no_current_type = false;
            }
          }
        } else if (callee == nullptr &&
                   !parent_intf_set.count(insn_method->get_class())) {
          for (const auto& type :
               insn_method->get_proto()->get_args()->get_type_list()) {
            if (parent_intf_set.count(type)) {
              no_current_type = false;
            }
          }
        } else {
          no_current_type = false;
        }
        if (no_current_type) {
          continue;
        }
      }
      // Recusive check every methods accessed from <init>.
      bool keep_going =
          get_ifields_read(whitelist_method_names, parent_intf_set, ifield_cls,
                           callee, blacklist_ifields, visited);
      if (!keep_going) {
        return false;
      }
    }
  }
  return true;
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
ConcurrentSet<DexField*> get_ifields_read_in_callees(
    const Scope& scope,
    const std::unordered_set<std::string>& whitelist_method_names) {
  ConcurrentSet<DexField*> return_ifields;
  TypeSystem ts(scope);
  walk::parallel::classes(scope, [&return_ifields, &ts,
                                  &whitelist_method_names](DexClass* cls) {
    if (cls->is_external()) {
      return;
    }
    auto ctors = cls->get_ctors();
    if (ctors.size() != 1 || cls->get_ifields().size() == 0) {
      // We are not inlining ifields in multi-ctors class so can also ignore
      // them here.
      // Also no need to proceed if there is no ifields for a class.
      return;
    }
    auto ctor = ctors[0];
    auto code = ctor->get_code();
    if (code != nullptr) {
      code->build_cfg(/* editable */ false);
      auto& cfg = code->cfg();
      cfg.calculate_exit_block();
      check_this::ThisObjectAnalysis fixpoint(
          &cfg, ctor, code->get_param_instructions().begin()->insn->dest());
      fixpoint.run(check_this::ThisEnvironment());
      // Only check on methods called with this object as arguments.
      auto check_methods = fixpoint.collect_method_called_on_this();
      if (!check_methods) {
        // This object escaped to heap, blacklist all.
        for (const auto& field : cls->get_ifields()) {
          return_ifields.emplace(field);
        }
        return;
      }
      if (check_methods->size() > 0) {
        std::unordered_set<const DexMethod*> visited;
        const auto& parent_chain = ts.parent_chain(cls->get_type());
        std::unordered_set<const DexType*> parent_intf_set{parent_chain.begin(),
                                                           parent_chain.end()};
        const auto& intf_set = ts.get_implemented_interfaces(cls->get_type());
        parent_intf_set.insert(intf_set.begin(), intf_set.end());
        for (const auto method : *check_methods) {
          bool keep_going =
              get_ifields_read(whitelist_method_names, parent_intf_set, cls,
                               method, &return_ifields, &visited);
          if (!keep_going) {
            break;
          }
        }
      }
    }
  });
  return return_ifields;
}

cp::EligibleIfields gather_ifield_candidates(
    const Scope& scope,
    const std::unordered_set<std::string>& whitelist_method_names) {
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
        if (field == nullptr || (method::is_init(method) &&
                                 method->get_class() == field->get_class())) {
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
  auto blacklist_ifields =
      get_ifields_read_in_callees(scope, whitelist_method_names);
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
    if (field_type == cp::FieldType::STATIC && method::is_clinit(method)) {
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
        if (field_type == cp::FieldType::INSTANCE && method::is_init(method) &&
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

} // namespace

size_t FinalInlinePassV2::run(const Scope& scope, const Config& config) {
  try {
    auto wps = final_inline::analyze_and_simplify_clinits(scope);
    return inline_final_gets(scope, wps, config.black_list_types,
                             cp::FieldType::STATIC);
  } catch (final_inline::class_initialization_cycle& e) {
    std::cerr << e.what();
    return 0;
  }
}

size_t FinalInlinePassV2::run_inline_ifields(
    const Scope& scope,
    const cp::EligibleIfields& eligible_ifields,
    const Config& config) {
  auto wps = final_inline::analyze_and_simplify_inits(scope, eligible_ifields);
  return inline_final_gets(scope, wps, config.black_list_types,
                           cp::FieldType::INSTANCE);
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
    cp::EligibleIfields eligible_ifields =
        gather_ifield_candidates(scope, m_config.whitelist_method_names);
    inlined_ifields_count =
        run_inline_ifields(scope, eligible_ifields, m_config);
  }
  mgr.incr_metric("num_static_finals_inlined", inlined_sfields_count);
  mgr.incr_metric("num_instance_finals_inlined", inlined_ifields_count);
}

static FinalInlinePassV2 s_pass;
