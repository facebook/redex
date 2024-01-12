/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FinalInlineV2.h"

#include <boost/variant.hpp>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <sparta/WeakTopologicalOrdering.h>

#include "CFGMutation.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "IFieldAnalysisUtil.h"
#include "LocalDce.h"
#include "PassManager.h"
#include "Shrinker.h"
#include "Timer.h"
#include "Trace.h"
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

using namespace sparta;

namespace {

std::ostream& operator<<(std::ostream& o,
                         const sparta::WtoComponent<DexClass*>& c) {
  if (c.is_scc()) {
    o << "(" << show(c.head_node());
    for (const auto& sub : c) {
      o << " " << sub;
    }
    o << ")";
  } else {
    o << show(c.head_node());
  }
  return o;
}

auto compute_deps(const Scope& scope,
                  const std::unordered_set<const DexClass*>& scope_set) {
  ConcurrentMap<DexClass*, std::vector<DexClass*>> deps_parallel;
  ConcurrentMap<DexClass*, std::vector<DexClass*>> reverse_deps_parallel;
  ConcurrentSet<DexClass*> is_target;
  ConcurrentSet<DexClass*> maybe_roots;
  ConcurrentSet<DexClass*> all;
  walk::parallel::classes(scope, [&](DexClass* cls) {
    std::vector<DexClass*> deps_vec;
    auto add_dep = [&](auto* dependee_cls) {
      if (dependee_cls == nullptr || dependee_cls == cls ||
          scope_set.count(dependee_cls) == 0) {
        return;
      }
      reverse_deps_parallel.update(
          dependee_cls, [&](auto&, auto& v, auto) { v.push_back(cls); });
      maybe_roots.insert(dependee_cls);
      deps_vec.push_back(dependee_cls);
    };

    // A superclass must be initialized before a subclass.
    //
    // We are not considering externals here. This should be fine, as
    // a chain internal <- external <- internal should not exist.
    {
      auto super_class = type_class_internal(cls->get_super_class());
      add_dep(super_class);
    }

    auto clinit = cls->get_clinit();
    if (clinit != nullptr && clinit->get_code() != nullptr) {
      editable_cfg_adapter::iterate_with_iterator(
          clinit->get_code(), [&](const IRList::iterator& it) {
            auto insn = it->insn;
            if (opcode::is_an_sfield_op(insn->opcode())) {
              add_dep(type_class(insn->get_field()->get_class()));
            } else if (opcode::is_invoke_static(insn->opcode())) {
              add_dep(type_class(insn->get_method()->get_class()));
            } else if (opcode::is_new_instance(insn->opcode())) {
              add_dep(type_class(insn->get_type()));
            }
            return editable_cfg_adapter::LOOP_CONTINUE;
          });
    }

    if (!deps_vec.empty()) {
      is_target.insert(cls);
      deps_parallel.emplace(cls, std::move(deps_vec));
    } else {
      // Something with no deps - make it a root so it gets visited.
      maybe_roots.insert(cls);
    }
    all.insert(cls);
  });
  std::unordered_map<DexClass*, std::vector<DexClass*>> deps;
  for (auto& kv : deps_parallel) {
    deps[kv.first] = std::move(kv.second);
  }
  std::unordered_map<DexClass*, std::vector<DexClass*>> reverse_deps;
  for (auto& kv : reverse_deps_parallel) {
    reverse_deps[kv.first] = std::move(kv.second);
  }

  std::vector<DexClass*> roots;
  std::copy_if(maybe_roots.begin(), maybe_roots.end(),
               std::back_inserter(roots),
               [&](auto* cls) { return is_target.count_unsafe(cls) == 0; });
  return std::make_tuple(std::move(deps), std::move(reverse_deps),
                         std::move(roots), all.size());
}

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
Scope reverse_tsort_by_clinit_deps(const Scope& scope, size_t& init_cycles) {
  Timer timer{"reverse_tsort_by_clinit_deps"};

  std::unordered_set<const DexClass*> scope_set(scope.begin(), scope.end());

  // Collect data for WTO.
  // NOTE: Doing this already also as reverse so we don't have to do that later.
  auto [deps, reverse_deps, roots, all_cnt] = compute_deps(scope, scope_set);

  // NOTE: Using nullptr for root node.
  auto wto = sparta::WeakTopologicalOrdering<DexClass*>(
      nullptr,
      [&roots = roots, &reverse_deps = reverse_deps](DexClass* const& cls) {
        if (cls == nullptr) {
          return roots;
        }

        auto it = reverse_deps.find(cls);
        if (it == reverse_deps.end()) {
          return std::vector<DexClass*>();
        }

        return it->second;
      });

  auto it = wto.begin();
  auto it_end = wto.end();

  redex_assert(it != it_end);
  redex_assert(it->is_vertex());
  redex_assert(it->head_node() == nullptr);
  ++it;

  Scope result;
  std::unordered_set<DexClass*> taken;

  for (; it != it_end; ++it) {
    if (it->is_scc()) {
      // Cycle...
      ++init_cycles;

      TRACE(FINALINLINE, 1, "Init cycle detected in %s",
            [&]() {
              std::ostringstream oss;
              oss << *it;
              return oss.str();
            }()
                .c_str());

      continue;
    }

    auto* cls = it->head_node();
    auto deps_it = deps.find(cls);
    if (deps_it != deps.end() &&
        !std::all_of(deps_it->second.begin(), deps_it->second.end(),
                     [&](auto* cls) { return taken.count(cls) != 0; })) {
      TRACE(FINALINLINE, 1, "Skipping %s because of missing deps", SHOW(cls));
      continue;
    }

    result.emplace_back(cls);
    taken.insert(cls);
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
Scope reverse_tsort_by_init_deps(const Scope& scope, size_t& possible_cycles) {
  std::unordered_set<const DexClass*> scope_set(scope.begin(), scope.end());
  Scope result;
  std::unordered_set<const DexClass*> visiting;
  std::unordered_set<const DexClass*> visited;
  std::function<void(DexClass*)> visit = [&](DexClass* cls) {
    if (visited.count(cls) != 0 || scope_set.count(cls) == 0) {
      return;
    }
    if (visiting.count(cls) != 0) {
      ++possible_cycles;
      TRACE(FINALINLINE, 1, "Possible class init cycle (could be benign):");
      for (auto visiting_cls : visiting) {
        TRACE(FINALINLINE, 1, "  %s", SHOW(visiting_cls));
      }
      TRACE(FINALINLINE, 1, "  %s", SHOW(cls));
      if (!traceEnabled(FINALINLINE, 1)) {
        TRACE(FINALINLINE, 0,
              "WARNING: Possible class init cycle found in FinalInlineV2. To "
              "check re-run with TRACE=FINALINLINE:1.\n");
      }
      return;
    }
    visiting.emplace(cls);
    const auto& ctors = cls->get_ctors();
    if (ctors.size() == 1) {
      auto ctor = ctors[0];
      if (ctor != nullptr && ctor->get_code() != nullptr) {
        editable_cfg_adapter::iterate_with_iterator(
            ctor->get_code(), [&](const IRList::iterator& it) {
              auto insn = it->insn;
              if (opcode::is_an_iget(insn->opcode())) {
                auto dependee_cls = type_class(insn->get_field()->get_class());
                if (dependee_cls == nullptr || dependee_cls == cls) {
                  return editable_cfg_adapter::LOOP_CONTINUE;
                }
                visit(dependee_cls);
              }
              return editable_cfg_adapter::LOOP_CONTINUE;
            });
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
                                cp::ConstantClassObjectAnalyzer,
                                cp::PrimitiveAnalyzer>;

using CombinedInitAnalyzer =
    InstructionAnalyzerCombiner<cp::InitFieldAnalyzer,
                                cp::WholeProgramAwareAnalyzer,
                                cp::StringAnalyzer,
                                cp::ConstantClassObjectAnalyzer,
                                cp::PrimitiveAnalyzer>;
/*
 * Converts a ConstantValue into its equivalent encoded_value. Returns null if
 * no such encoding is known.
 */
class encoding_visitor
    : public boost::static_visitor<std::unique_ptr<DexEncodedValue>> {
 public:
  explicit encoding_visitor(const DexField* field,
                            const XStoreRefs* xstores,
                            const DexType* declaring_type)
      : m_field(field), m_xstores(xstores), m_declaring_type(declaring_type) {}

  std::unique_ptr<DexEncodedValue> operator()(
      const SignedConstantDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return nullptr;
    }
    auto ev = DexEncodedValue::zero_for_type(m_field->get_type());
    ev->value(static_cast<uint64_t>(*cst));
    return ev;
  }

  std::unique_ptr<DexEncodedValue> operator()(const StringDomain& dom) const {
    auto cst = dom.get_constant();

    // Older DalvikVM handles only two types of classes:
    // https://android.googlesource.com/platform/dalvik.git/+/android-4.3_r3/vm/oo/Class.cpp#3846
    // Without this checking, we may mistakenly accept a "const-string" and
    // "sput-object Ljava/lang/CharSequence;" pair. Such pair can cause a
    // libdvm.so abort with "Bogus static initialization".
    if (cst && m_field->get_type() == type::java_lang_String()) {
      return std::unique_ptr<DexEncodedValue>(new DexEncodedValueString(*cst));
    } else {
      return nullptr;
    }
  }

  std::unique_ptr<DexEncodedValue> operator()(
      const ConstantClassObjectDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return nullptr;
    }
    if (m_field->get_type() != type::java_lang_Class()) {
      // See above: There's a limitation in older DalvikVMs
      return nullptr;
    }
    auto type = const_cast<DexType*>(*cst);
    if (!m_xstores || m_xstores->illegal_ref(m_declaring_type, type)) {
      return nullptr;
    }
    return std::unique_ptr<DexEncodedValue>(new DexEncodedValueType(type));
  }

  template <typename Domain>
  std::unique_ptr<DexEncodedValue> operator()(const Domain&) const {
    return nullptr;
  }

 private:
  const DexField* m_field;
  const XStoreRefs* m_xstores;
  const DexType* m_declaring_type;
};

class ClassInitStrategy final : public call_graph::SingleCalleeStrategy {
 public:
  explicit ClassInitStrategy(
      const method_override_graph::Graph& method_override_graph,
      const Scope& scope)
      : call_graph::SingleCalleeStrategy(method_override_graph, scope) {}

  call_graph::RootAndDynamic get_roots() const override {
    call_graph::RootAndDynamic root_and_dynamic;
    auto& roots = root_and_dynamic.roots;

    walk::methods(m_scope, [&](DexMethod* method) {
      if (method::is_clinit(method)) {
        roots.emplace_back(method);
      }
    });
    return root_and_dynamic;
  }
};

void encode_values(DexClass* cls,
                   const FieldEnvironment& field_env,
                   const PatriciaTreeSet<const DexFieldRef*>& blocklist,
                   const XStoreRefs* xstores) {
  for (auto* field : cls->get_sfields()) {
    if (blocklist.contains(field)) {
      continue;
    }
    auto value = field_env.get(field);
    auto encoded_value = ConstantValue::apply_visitor(
        encoding_visitor(field, xstores, cls->get_type()), value);
    if (encoded_value == nullptr) {
      continue;
    }
    field->set_value(std::move(encoded_value));
    TRACE(FINALINLINE, 2, "Found encodable field: %s %s", SHOW(field),
          SHOW(value));
  }
}

} // namespace

namespace final_inline {

call_graph::Graph build_class_init_graph(const Scope& scope) {
  Timer t("Build class init graph");
  auto graph = call_graph::Graph(
      ClassInitStrategy(*method_override_graph::build_graph(scope), scope));
  return graph;
}

StaticFieldReadAnalysis::StaticFieldReadAnalysis(
    const call_graph::Graph& call_graph,
    const std::unordered_set<std::string>& allowed_opaque_callee_names)
    : m_graph(call_graph) {

  // By default, the analysis gives up when it sees a true virtual callee.
  // However, we can allow some methods to be treated as if no field is read
  // from the callees so the analysis gives up less often.
  for (const auto& name : allowed_opaque_callee_names) {
    DexMethodRef* callee = DexMethod::get_method(name);
    if (callee) {
      m_allowed_opaque_callees.emplace(callee);
    }
  }
}

/*
 * If a field is both read and written to in its initializer, then we can
 * update its encoded value with the value at exit only if the reads (sgets) are
 * are dominated by the writes (sputs) -- otherwise we may change program
 * semantics. Checking for dominance takes some work, and static fields are
 * rarely read in their class' <clinit>, so we simply avoid inlining all fields
 * that are read in their class' <clinit>.
 *
 * This analysis is an interprocedural analysis that collects all static field
 * reads from the current method. Technically there are other opcodes that
 * triggers more <clinit>s, which can also read from a field. To make this fully
 * sound, we need to account for potential class loads as well.
 */

StaticFieldReadAnalysis::Result StaticFieldReadAnalysis::analyze(
    const DexMethod* method) {
  std::unordered_set<const DexMethod*> pending;

  Result last = Result::bottom();
  while (true) {
    Result new_result = analyze(method, pending);
    if (pending.count(method) == 0 || new_result == last) {
      pending.erase(method);
      m_finalized.emplace(method);
      return new_result;
    } else {
      last = new_result;
    }
  }
}

StaticFieldReadAnalysis::Result StaticFieldReadAnalysis::analyze(
    const DexMethod* method,
    std::unordered_set<const DexMethod*>& pending_methods) {

  if (!method) {
    return {};
  }

  if (m_finalized.count(method)) {
    return m_summaries.at(method);
  }

  auto code = const_cast<IRCode*>(method->get_code());
  if (!code) {
    return {};
  }

  Result ret{};
  editable_cfg_adapter::iterate_with_iterator(
      code, [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (opcode::is_an_sget(insn->opcode())) {
          ret.add(insn->get_field());
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });

  pending_methods.emplace(method);
  m_summaries[method] = ret;

  bool callee_pending = false;

  editable_cfg_adapter::iterate_with_iterator(
      code, [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (opcode::is_an_invoke(insn->opcode())) {
          auto callee_method_def = resolve_method(
              insn->get_method(), opcode_to_search(insn), method);
          if (!callee_method_def || callee_method_def->is_external() ||
              !callee_method_def->is_concrete() ||
              m_allowed_opaque_callees.count(callee_method_def)) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          auto callees = resolve_callees_in_graph(m_graph, method, insn);
          if (callees.empty()) {
            TRACE(FINALINLINE, 2, "%s has opaque callees %s", SHOW(method),
                  SHOW(insn->get_method()));
            ret = Result::top();
            callee_pending = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }

          for (const DexMethod* callee : callees) {
            Result callee_result;
            if (pending_methods.count(callee)) {
              callee_pending = true;
              callee_result = m_summaries.at(callee);
            } else {
              callee_result = analyze(callee, pending_methods);
            }
            ret.join_with(callee_result);
            if (ret.is_top()) {
              callee_pending = false;
              return editable_cfg_adapter::LOOP_BREAK;
            }
            if (pending_methods.count(callee)) {
              callee_pending = true;
            }
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  if (!callee_pending) {
    pending_methods.erase(method);
  }
  m_summaries[method] = ret;
  return ret;
}

/*
 * This method determines the values of the static fields after the <clinit>
 * has finished running and generates their encoded_value equivalents.
 *
 * Additionally, for static final fields, this method collects and returns them
 * as part of the WholeProgramState object.
 */
cp::WholeProgramState analyze_and_simplify_clinits(
    const Scope& scope,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const XStoreRefs* xstores,
    const std::unordered_set<const DexType*>& blocklist_types,
    const std::unordered_set<std::string>& allowed_opaque_callee_names,
    size_t& init_cycles) {
  const std::unordered_set<DexMethodRef*> pure_methods = get_pure_methods();
  cp::WholeProgramState wps(blocklist_types);

  auto method_override_graph = method_override_graph::build_graph(scope);
  auto graph =
      call_graph::Graph(ClassInitStrategy(*method_override_graph, scope));
  StaticFieldReadAnalysis analysis(graph, allowed_opaque_callee_names);

  cp::Transform::RuntimeCache runtime_cache{};

  for (DexClass* cls : reverse_tsort_by_clinit_deps(scope, init_cycles)) {
    ConstantEnvironment env;
    cp::set_encoded_values(cls, &env);
    auto clinit = cls->get_clinit();
    if (clinit != nullptr && clinit->get_code() != nullptr) {
      auto* code = clinit->get_code();
      {
        auto& cfg = code->cfg();
        cfg.calculate_exit_block();
        constant_propagation::WholeProgramStateAccessor wps_accessor(wps);
        cp::intraprocedural::FixpointIterator intra_cp(
            cfg,
            CombinedAnalyzer(cls->get_type(), &wps_accessor, nullptr, nullptr,
                             nullptr));
        intra_cp.run(env);
        env = intra_cp.get_exit_state_at(cfg.exit_block());

        // Generate the new encoded_values and re-run the analysis.
        StaticFieldReadAnalysis::Result res = analysis.analyze(clinit);

        if (res.is_bottom() || res.is_top()) {
          TRACE(FINALINLINE, 1, "Skipped encoding for class %s.", SHOW(cls));
        } else {
          encode_values(cls, env.get_field_environment(), res.elements(),
                        xstores);
        }
        auto fresh_env = ConstantEnvironment();
        cp::set_encoded_values(cls, &fresh_env);
        intra_cp.run(fresh_env);

        // Detect any field writes made redundant by the new encoded_values and
        // remove those sputs.
        cp::Transform::Config transform_config;
        transform_config.class_under_init = cls->get_type();
        cp::Transform(transform_config, &runtime_cache)
            .legacy_apply_constants_and_prune_unreachable(
                intra_cp, wps, cfg, xstores, cls->get_type());
        // Delete the instructions rendered dead by the removal of those sputs.
        LocalDce(&init_classes_with_side_effects, pure_methods)
            .dce(cfg, /* normalize_new_instances */ true, clinit->get_class());
      }
      // If the clinit is empty now, delete it.
      if (method::is_trivial_clinit(*code)) {
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
    const Scope& scope,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const XStoreRefs* xstores,
    const std::unordered_set<const DexType*>& blocklist_types,
    const cp::EligibleIfields& eligible_ifields,
    size_t& possible_cycles) {
  const std::unordered_set<DexMethodRef*> pure_methods = get_pure_methods();
  cp::WholeProgramState wps(blocklist_types);
  for (DexClass* cls : reverse_tsort_by_init_deps(scope, possible_cycles)) {
    if (cls->is_external()) {
      continue;
    }
    ConstantEnvironment env;
    auto ctors = cls->get_ctors();
    if (ctors.size() > 1) {
      continue;
    }
    if (ctors.size() == 1) {
      bool has_same_type_arg = false;
      auto cls_type = cls->get_type();
      for (auto arg_type : *(ctors[0]->get_proto()->get_args())) {
        if (arg_type == cls_type) {
          has_same_type_arg = true;
        }
      }
      if (has_same_type_arg) {
        continue;
      }
    }
    cp::set_ifield_values(cls, eligible_ifields, &env);
    if (ctors.size() == 1) {
      auto ctor = ctors[0];
      if (ctor->get_code() != nullptr) {
        auto* code = ctor->get_code();
        auto& cfg = code->cfg();
        cfg.calculate_exit_block();
        constant_propagation::WholeProgramStateAccessor wps_accessor(wps);
        cp::intraprocedural::FixpointIterator intra_cp(
            cfg,
            CombinedInitAnalyzer(cls->get_type(), &wps_accessor, nullptr,
                                 nullptr, nullptr));
        intra_cp.run(env);
        env = intra_cp.get_exit_state_at(cfg.exit_block());

        // Remove redundant iputs in inits
        cp::Transform::Config transform_config;
        transform_config.class_under_init = cls->get_type();
        cp::Transform(transform_config)
            .legacy_apply_constants_and_prune_unreachable(
                intra_cp, wps, cfg, xstores, cls->get_type());
        // Delete the instructions rendered dead by the removal of those iputs.
        LocalDce(&init_classes_with_side_effects, pure_methods)
            .dce(cfg, /* normalize_new_instances */ true, ctor->get_class());
      }
    }
    wps.collect_instance_finals(cls, eligible_ifields,
                                env.get_field_environment());
  }
  return wps;
}

} // namespace final_inline

namespace {

FinalInlinePassV2::Stats inline_final_gets(
    std::optional<DexStoresVector*> stores,
    const Scope& scope,
    int min_sdk,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const XStoreRefs* xstores,
    const cp::WholeProgramState& wps,
    const std::unordered_set<const DexType*>& blocklist_types,
    cp::FieldType field_type) {
  std::atomic<size_t> inlined_count{0};
  std::atomic<size_t> init_classes{0};
  using namespace shrinker;

  ShrinkerConfig shrinker_config;
  shrinker_config.run_const_prop = true;
  shrinker_config.run_cse = true;
  shrinker_config.run_copy_prop = true;
  shrinker_config.run_local_dce = true;
  shrinker_config.compute_pure_methods = false;

  auto maybe_shrinker =
      stores ? std::make_optional<Shrinker>(**stores, scope,
                                            init_classes_with_side_effects,
                                            shrinker_config, min_sdk)
             : std::nullopt;

  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (field_type == cp::FieldType::STATIC && method::is_clinit(method)) {
      return;
    }
    cfg::CFGMutation mutation(code.cfg());
    size_t replacements = 0;
    for (auto block : code.cfg().blocks()) {
      auto ii = InstructionIterable(block);
      for (auto it = ii.begin(); it != ii.end(); it++) {
        auto insn = it->insn;
        auto op = insn->opcode();
        if (opcode::is_an_iget(op) || opcode::is_an_sget(op)) {
          auto field = resolve_field(insn->get_field());
          if (field == nullptr || blocklist_types.count(field->get_class())) {
            continue;
          }
          if (field_type == cp::FieldType::INSTANCE &&
              method::is_init(method) &&
              method->get_class() == field->get_class()) {
            // Don't propagate a field's value in ctors of its class with
            // value after ctor finished.
            continue;
          }
          auto cfg_it = block->to_cfg_instruction_iterator(it);
          auto replacement = ConstantValue::apply_visitor(
              cp::value_to_instruction_visitor(
                  code.cfg().move_result_of(cfg_it)->insn,
                  xstores,
                  method->get_class()),
              wps.get_field_value(field));
          if (replacement.empty()) {
            continue;
          }
          auto init_class_insn =
              opcode::is_an_sget(op)
                  ? init_classes_with_side_effects.create_init_class_insn(
                        field->get_class())
                  : nullptr;
          if (init_class_insn) {
            replacement.insert(replacement.begin(), init_class_insn);
            init_classes++;
          }
          mutation.replace(cfg_it, replacement);
          replacements++;
        }
      }
    }
    mutation.flush();
    if (replacements > 0 && maybe_shrinker) {
      maybe_shrinker->shrink_method(method);
    }
    inlined_count.fetch_add(replacements);
  });
  return {(size_t)inlined_count, (size_t)init_classes};
}

} // namespace

FinalInlinePassV2::Stats FinalInlinePassV2::run(
    const Scope& scope,
    int min_sdk,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const XStoreRefs* xstores,
    const Config& config,
    std::optional<DexStoresVector*> stores) {
  size_t clinit_cycles = 0;
  auto wps = final_inline::analyze_and_simplify_clinits(
      scope, init_classes_with_side_effects, xstores, config.blocklist_types,
      {}, clinit_cycles);
  auto res = inline_final_gets(stores, scope, min_sdk,
                               init_classes_with_side_effects, xstores, wps,
                               config.blocklist_types, cp::FieldType::STATIC);
  return {res.inlined_count, res.init_classes, clinit_cycles};
}

FinalInlinePassV2::Stats FinalInlinePassV2::run_inline_ifields(
    const Scope& scope,
    int min_sdk,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    const XStoreRefs* xstores,
    const cp::EligibleIfields& eligible_ifields,
    const Config& config,
    std::optional<DexStoresVector*> stores) {
  size_t possible_cycles = 0;
  auto wps = final_inline::analyze_and_simplify_inits(
      scope, init_classes_with_side_effects, xstores, config.blocklist_types,
      eligible_ifields, possible_cycles);
  auto ret = inline_final_gets(stores, scope, min_sdk,
                               init_classes_with_side_effects, xstores, wps,
                               config.blocklist_types, cp::FieldType::INSTANCE);
  ret.possible_cycles = possible_cycles;
  return ret;
}

void FinalInlinePassV2::run_pass(DexStoresVector& stores,
                                 ConfigFiles& conf,
                                 PassManager& mgr) {
  always_assert_log(
      !mgr.init_class_lowering_has_run(),
      "Implementation limitation: FinalInlinePassV2 could introduce new "
      "init-class instructions.");
  auto scope = build_class_scope(stores);
  auto min_sdk = mgr.get_redex_options().min_sdk;
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());
  XStoreRefs xstores(stores);
  auto sfield_stats = run(scope, min_sdk, init_classes_with_side_effects,
                          &xstores, m_config, &stores);
  FinalInlinePassV2::Stats ifield_stats{};
  if (m_config.inline_instance_field) {
    cp::EligibleIfields eligible_ifields =
        cp::gather_safely_inferable_ifield_candidates(
            scope, m_config.allowlist_method_names);
    ifield_stats =
        run_inline_ifields(scope, min_sdk, init_classes_with_side_effects,
                           &xstores, eligible_ifields, m_config, &stores);
    always_assert(ifield_stats.init_classes == 0);
  }
  mgr.incr_metric("num_static_finals_inlined", sfield_stats.inlined_count);
  mgr.incr_metric("num_instance_finals_inlined", ifield_stats.inlined_count);
  mgr.incr_metric("num_init_classes", sfield_stats.init_classes);
  mgr.incr_metric("num_possible_clinit_cycles", sfield_stats.possible_cycles);
  mgr.incr_metric("num_possible_init_cycles", ifield_stats.possible_cycles);
}

static FinalInlinePassV2 s_pass;
