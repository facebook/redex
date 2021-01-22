/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveBuilderPattern.h"

#include <boost/regex.hpp>

#include "BuilderAnalysis.h"
#include "BuilderTransform.h"
#include "CommonSubexpressionElimination.h"
#include "ConfigFiles.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "ConstantPropagationWholeProgramState.h"
#include "CopyPropagation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "LocalDce.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace builder_pattern {

namespace {

/**
 * Example: Lcom/facebook/RandomClassName; -> RandomClassName
 */
std::string only_class_name(const DexType* type) {
  std::string type_str(type->c_str());
  size_t package_delim = type_str.rfind('/');
  always_assert(package_delim != std::string::npos);

  size_t class_start = package_delim + 1;
  return type_str.substr(class_start, type_str.size() - package_delim - 2);
}

std::unordered_set<DexType*> get_associated_buildees(
    const std::unordered_set<const DexType*>& builders) {

  std::unordered_set<DexType*> buildees;
  for (const auto& builder : builders) {
    const std::string& builder_name = builder->str();
    std::string buildee_name =
        builder_name.substr(0, builder_name.size() - 9) + ";";

    auto type = DexType::get_type(buildee_name.c_str());
    if (type) {
      buildees.emplace(type);
    }
  }

  return buildees;
}

class RemoveClasses {
 public:
  RemoveClasses(const DexType* super_cls,
                const Scope& scope,
                const inliner::InlinerConfig& inliner_config,
                const std::vector<DexType*>& blocklist,
                bool propagate_escape_results,
                DexStoresVector& stores)
      : m_root(super_cls),
        m_scope(scope),
        m_blocklist(blocklist),
        m_type_system(scope),
        m_propagate_escape_results(propagate_escape_results),
        m_transform(scope, m_type_system, super_cls, inliner_config, stores),
        m_stores(stores) {
    gather_classes();
  }

  void optimize() {
    collect_excluded_types();

    if (m_root != type::java_lang_Object()) {
      // We can't inline a method that has super calls.
      for (const DexType* builder : m_classes) {
        if (!m_transform.inline_super_calls_and_ctors(builder)) {
          TRACE(BLD_PATTERN, 2,
                "Excluding type %s since we cannot inline super calls for all "
                "methods",
                SHOW(builder));
          m_excluded_types.emplace(builder);
        }
      }
    }

    update_usage();
  }

  void cleanup() { m_transform.cleanup(); }

  void print_stats(PassManager& mgr) {
    auto root_name = only_class_name(m_root);
    mgr.set_metric(root_name + "_total_classes", m_classes.size());
    mgr.set_metric(root_name + "_num_classes_excluded",
                   m_excluded_types.size());
    mgr.set_metric(root_name + "_num_total_usages", m_num_usages);
    mgr.set_metric(root_name + "_num_removed_usages", m_num_removed_usages);
    for (const auto& type : m_excluded_types) {
      TRACE(BLD_PATTERN, 2, "Excluded type: %s", SHOW(type));
    }
    mgr.set_metric(root_name + "_num_classes_removed", m_removed_types.size());
    for (const auto& type : m_removed_types) {
      TRACE(BLD_PATTERN, 2, "Removed type: %s", SHOW(type));
    }
  }

 private:
  void gather_classes() {
    const TypeSet& subclasses = m_type_system.get_children(m_root);
    auto* object_type = type::java_lang_Object();
    boost::regex re("\\$Builder;$");

    // We are only tackling leaf classes.
    for (const DexType* type : subclasses) {
      if (m_type_system.get_children(type).empty()) {
        if (m_root != object_type || boost::regex_search(type->c_str(), re)) {
          m_classes.emplace(type);
        }
      }
    }
  }

  void update_usage() {
    auto buildee_types = get_associated_buildees(m_classes);

    std::mutex methods_mutex;
    std::vector<DexMethod*> methods;

    walk::parallel::methods(m_scope, [&](DexMethod* method) {
      if (!method || !method->get_code()) {
        return;
      }

      if (m_classes.count(method->get_class()) ||
          buildee_types.count(method->get_class())) {
        // Skip builder and associated buildee methods.
        return;
      }

      BuilderAnalysis analysis(m_classes, m_excluded_types, method);
      analysis.run_analysis();
      if (analysis.has_usage()) {
        std::unique_lock<std::mutex> lock{methods_mutex};
        methods.push_back(method);
      }
    });

    if (methods.empty()) {
      return;
    }
    std::sort(methods.begin(), methods.end(), compare_dexmethods);

    for (auto method : methods) {
      BuilderAnalysis analysis(m_classes, m_excluded_types, method);

      bool are_builders_to_remove =
          inline_builders_and_check_method(method, &analysis);
      m_num_usages += analysis.get_total_num_usages();

      if (!are_builders_to_remove) {
        continue;
      }

      // When we get here we know that we can remove the builders.
      m_num_removed_usages += analysis.get_num_usages();

      auto removed_types = analysis.get_instantiated_types();
      TRACE(BLD_PATTERN, 2, "Removed following builders from %s", SHOW(method));
      for (const auto& type : removed_types) {
        m_removed_types.emplace(type);
        TRACE(BLD_PATTERN, 2, "\t %s", SHOW(type));
      }

      m_transform.replace_fields(analysis.get_usage(), method);
    }

    shrink_methods(methods);
  }

  void shrink_methods(const std::vector<DexMethod*>& methods) {
    // Run shrinking opts to optimize the changed methods.

    XStoreRefs xstores(m_stores);
    std::unordered_set<DexMethodRef*> pure_methods; // Don't assume anything;
    std::unordered_set<DexString*> finalish_field_names; // Don't assume
                                                         // anything;
    cse_impl::SharedState shared_state(pure_methods, finalish_field_names);

    auto post_process = [&](DexMethod* method) {
      auto code = method->get_code();

      {
        if (code->editable_cfg_built()) {
          code->clear_cfg();
        }
        code->build_cfg(/*editable=*/false);
        constant_propagation::intraprocedural::FixpointIterator fp_iter(
            code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
        fp_iter.run(ConstantEnvironment());
        constant_propagation::Transform::Config config;
        constant_propagation::Transform tf(config);
        tf.apply_on_uneditable_cfg(fp_iter,
                                   constant_propagation::WholeProgramState(),
                                   code, &xstores, method->get_class());
        code->clear_cfg();
      }

      always_assert(!code->editable_cfg_built());
      cfg::ScopedCFG cfg(code);
      cfg->calculate_exit_block();
      {
        constant_propagation::intraprocedural::FixpointIterator fp_iter(
            *cfg, constant_propagation::ConstantPrimitiveAnalyzer());
        fp_iter.run(ConstantEnvironment());
        constant_propagation::Transform::Config config;
        constant_propagation::Transform tf(config);
        tf.apply(fp_iter, *cfg, method, &xstores);
      }

      {
        cse_impl::CommonSubexpressionElimination cse(
            &shared_state, *cfg, is_static(method),
            method::is_init(method) || method::is_clinit(method),
            method->get_class(), method->get_proto()->get_args());
        cse.patch();
      }

      {
        copy_propagation_impl::Config copy_prop_config;
        copy_prop_config.eliminate_const_classes = false;
        copy_prop_config.eliminate_const_strings = false;
        copy_prop_config.static_finals = false;
        copy_propagation_impl::CopyPropagation copy_propagation(
            copy_prop_config);
        copy_propagation.run(code, method);
      }

      {
        LocalDce dce(pure_methods, /* no mog */ nullptr,
                     /*may_allocate_registers=*/true);
        dce.dce(*cfg);
      }
    };

    // Walkers are over classes, so need to do this "manually."
    auto wq = workqueue_foreach<DexMethod*>(post_process);
    for (auto* m : methods) {
      wq.add_item(m);
    }
    wq.run_all();
  }

  void collect_excluded_types() {
    walk::fields(m_scope, [&](DexField* field) {
      auto type = field->get_type();
      if (m_classes.count(type)) {
        TRACE(BLD_PATTERN, 2,
              "Excluding type since it is stored in a field: %s", SHOW(type));
        m_excluded_types.emplace(type);
      }
    });

    for (DexType* type : m_blocklist) {
      if (m_classes.count(type)) {
        TRACE(BLD_PATTERN, 2,
              "Excluding type since it was in the blocklist: %s", SHOW(type));
        m_excluded_types.emplace(type);
      }
    }
  }

  /**
   * Returns true if there are builders that we can remove from the current
   * method.
   */
  bool inline_builders_and_check_method(DexMethod* method,
                                        BuilderAnalysis* analysis) {
    bool builders_to_remove = false;

    // To be used for local excludes. We cleanup m_excluded_types at the end.
    std::unordered_set<const DexType*> local_excludes;

    std::unique_ptr<IRCode> original_code = nullptr;

    do {
      analysis->run_analysis();
      if (!analysis->has_usage()) {
        TRACE(BLD_PATTERN, 6, "No builder to remove from %s", SHOW(method));
        break;
      }
      if (original_code == nullptr) {
        // Keep a copy of the code, in order to restore it, if needed.
        original_code = std::make_unique<IRCode>(*method->get_code());
      }

      // First bind virtual callsites to the current implementation, if any,
      // in order to be able to inline them.
      auto vinvoke_to_instance = analysis->get_vinvokes_to_this_infered_type();
      m_transform.update_virtual_calls(vinvoke_to_instance);

      // Inline all methods that are either called on the builder instance
      // or take the builder as an argument, except for the ctors.
      std::unordered_set<IRInstruction*> to_inline =
          analysis->get_all_inlinable_insns();
      if (to_inline.empty()) {
        TRACE(BLD_PATTERN, 3,
              "Everything that could be inlined was inlined for %s",
              SHOW(method));

        // Check if any of the instance builder types cannot be removed.
        auto non_removable_types = analysis->non_removable_types();
        if (!non_removable_types.empty()) {
          for (DexType* type : non_removable_types) {
            if (m_excluded_types.count(type) == 0 &&
                !m_propagate_escape_results) {
              local_excludes.emplace(type);
            }

            m_excluded_types.emplace(type);
          }

          // Restore method and re-try. We will only
          // try removing non-excluded types.
          method->set_code(std::make_unique<IRCode>(*original_code));
          continue;
        } else {
          TRACE(BLD_PATTERN, 2,
                "Everything that could be inlined was inlined and none of "
                "the instances escape for %s",
                SHOW(method));
          analysis->print_usage();
          builders_to_remove = true;
          break;
        }
      }

      auto not_inlined_insns =
          m_transform.get_not_inlined_insns(method, to_inline);

      if (!not_inlined_insns.empty()) {
        auto to_eliminate =
            analysis->get_instantiated_types(&not_inlined_insns);
        for (const DexType* type : to_eliminate) {
          if (m_excluded_types.count(type) == 0 &&
              !m_propagate_escape_results) {
            local_excludes.emplace(type);
          }

          m_excluded_types.emplace(type);
        }

        if (not_inlined_insns.size() == to_inline.size()) {
          // Nothing left to do, since nothing was inlined.
          TRACE(BLD_PATTERN, 4, "Couldn't inline any of the methods in %s",
                SHOW(method));
          for (const auto& insn : not_inlined_insns) {
            TRACE(BLD_PATTERN, 5, "\t%s", SHOW(insn));
          }
          break;
        } else {
          // Restore method and re-try. We will only try inlining non-excluded
          // types.
          TRACE(BLD_PATTERN, 4, "Couldn't inline all the methods in %s",
                SHOW(method));
          for (const auto& insn : not_inlined_insns) {
            TRACE(BLD_PATTERN, 5, "\t%s", SHOW(insn));
          }
          method->set_code(std::make_unique<IRCode>(*original_code));
        }
      }

      // If we inlined everything, we still need to make sure we don't have
      // new  methods to inlined (for example from something that was inlined
      // in this step).
    } while (true);

    for (const DexType* type : local_excludes) {
      m_excluded_types.erase(type);
    }
    if (!builders_to_remove && original_code != nullptr) {
      method->set_code(std::move(original_code));
    }
    return builders_to_remove;
  }

  const DexType* m_root;
  const Scope& m_scope;
  const std::vector<DexType*>& m_blocklist;
  TypeSystem m_type_system;
  bool m_propagate_escape_results;
  BuilderTransform m_transform;
  std::unordered_set<const DexType*> m_classes;
  std::unordered_set<const DexType*> m_excluded_types;
  std::unordered_set<const DexType*> m_removed_types;
  size_t m_num_usages{0};
  size_t m_num_removed_usages{0};
  const DexStoresVector& m_stores;
};

} // namespace

void RemoveBuilderPatternPass::bind_config() {
  std::vector<DexType*> roots;
  bind("roots", {}, roots, Configurable::default_doc(),
       Configurable::bindflags::types::warn_if_unresolvable);
  bind("blocklist", {}, m_blocklist, Configurable::default_doc(),
       Configurable::bindflags::types::warn_if_unresolvable);
  bind("propagate_escape_results", true, m_propagate_escape_results);

  // TODO(T44502473): if we could pass a binding filter lambda instead of
  // bindflags, this could be more simply expressed
  after_configuration([this, roots] {
    auto object_type = type::java_lang_Object();
    m_roots.clear();
    for (const auto& root : roots) {
      if (!type_class(root)) continue;
      if (root != object_type) {
        auto super_cls = type_class(root)->get_super_class();
        if (super_cls != object_type) {
          fprintf(stderr,
                  "[builders]: %s isn't a valid root as it extends %s\n",
                  root->c_str(), super_cls->c_str());
          continue;
        }
      }
      m_roots.push_back(root);
    }
  });
}

void RemoveBuilderPatternPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  auto scope = build_class_scope(stores);

  for (const auto& root : m_roots) {
    RemoveClasses rm_builder_pattern(root, scope, conf.get_inliner_config(),
                                     m_blocklist, m_propagate_escape_results,
                                     stores);
    rm_builder_pattern.optimize();
    rm_builder_pattern.print_stats(mgr);
    rm_builder_pattern.cleanup();
  }
}

static RemoveBuilderPatternPass s_pass;

} // namespace builder_pattern
