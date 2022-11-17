/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUninstantiablesPass.h"

#include <cinttypes>

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "MethodDedup.h"
#include "NullPointerExceptionUtil.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

#include <boost/optional.hpp>
#include <iostream>

std::string RemoveUninstantiablesPass::m_proguard_usage_name = "";
std::istream* RemoveUninstantiablesPass::test_only_usage_file_input = nullptr;

namespace {

/// \return a new \c IRInstruction representing a `const` operation writing
/// literal \p lit into register \p dest.
IRInstruction* ir_const(uint32_t dest, int64_t lit) {
  auto insn = new IRInstruction(OPCODE_CONST);
  insn->set_dest(dest);
  insn->set_literal(lit);
  return insn;
}

/// \return a new \c IRInstruction representing a `throw` operation, throwing
/// the contents of register \p src.
IRInstruction* ir_throw(uint32_t src) {
  auto insn = new IRInstruction(OPCODE_THROW);
  insn->set_src(0, src);
  return insn;
}

/// \return a new \c IRInstruction representing a `check-cast` operation,
/// verifying that \p src is compatible with \p type.
IRInstruction* ir_check_cast(uint32_t src, DexType* type) {
  auto insn = new IRInstruction(OPCODE_CHECK_CAST);
  insn->set_src(0, src);
  insn->set_type(type);
  return insn;
}

/// \return a new \c IRInstruction representing a `move-result-pseudo-object`
/// operation.
IRInstruction* ir_move_result_pseudo_object(uint32_t dest) {
  auto insn = new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  insn->set_dest(dest);
  return insn;
}

struct VirtualScopeId {
  const DexString* name;
  DexProto* proto;
  static VirtualScopeId make(DexMethodRef* method) {
    return VirtualScopeId{method->get_name(), method->get_proto()};
  }
};

struct VirtualScopeIdHasher {
  size_t operator()(const VirtualScopeId& vs) const {
    return ((size_t)vs.name) * 27 + (size_t)vs.proto;
  }
};

bool operator==(const VirtualScopeId& a, const VirtualScopeId& b) {
  return a.name == b.name && a.proto == b.proto;
}

using VirtualScopeIdSet =
    std::unordered_set<VirtualScopeId, VirtualScopeIdHasher>;

// Helper analysis that determines if we need to keep the code of a method (or
// if it can never run)
class OverriddenVirtualScopesAnalysis {
 private:
  const std::unordered_set<DexType*>& m_scoped_uninstantiable_types;

  std::unordered_map<DexType*, VirtualScopeIdSet>
      m_transitively_defined_virtual_scopes;

  ConcurrentSet<DexType*> m_instantiated_types;
  ConcurrentSet<VirtualScopeId, VirtualScopeIdHasher>
      m_unresolved_super_invoked_virtual_scopes;
  ConcurrentSet<DexMethod*> m_resolved_super_invoked_methods;

  // This helper method initializes
  // m_transitively_defined_virtual_scopes for a particular type
  // finding all virtual scopes which are defined by itself, if actually
  // instantiated, or all instantiable children of the given type.
  void compute_transitively_defined_virtual_scope(
      const std::unordered_map<DexType*, std::unordered_set<DexType*>>&
          instantiable_children,
      const ConcurrentMap<const DexType*, VirtualScopeIdSet>&
          defined_virtual_scopes,
      DexType* t) {
    auto it = m_transitively_defined_virtual_scopes.find(t);
    if (it != m_transitively_defined_virtual_scopes.end()) {
      return;
    }
    auto& res = m_transitively_defined_virtual_scopes[t];
    if (is_instantiated(t)) {
      const auto& own_defined_virtual_scopes =
          defined_virtual_scopes.at_unsafe(t);
      res.insert(own_defined_virtual_scopes.begin(),
                 own_defined_virtual_scopes.end());
      return;
    }
    std::unordered_map<VirtualScopeId, size_t, VirtualScopeIdHasher> counted;
    auto children_it = instantiable_children.find(t);
    if (children_it != instantiable_children.end()) {
      const auto& children = children_it->second;
      for (auto child : children) {
        const auto& defined_virtual_scopes_of_child =
            defined_virtual_scopes.at_unsafe(child);
        for (const auto& virtual_scope : defined_virtual_scopes_of_child) {
          counted[virtual_scope]++;
        }
        compute_transitively_defined_virtual_scope(
            instantiable_children, defined_virtual_scopes, child);
        for (auto& virtual_scope :
             m_transitively_defined_virtual_scopes.at(child)) {
          if (!defined_virtual_scopes_of_child.count(virtual_scope)) {
            counted[virtual_scope]++;
          }
        }
      }
      auto children_size = children.size();
      for (auto& p : counted) {
        if (p.second == children_size) {
          res.insert(p.first);
        }
      }
    }
  }

  // Helper function that finds...
  // 1. all types that are actually instantiated via new-instance, and
  // 2. all targets of an invoke-super, i.e. methods that can be directly
  //    invoked even if overridden by all instantiable children
  void scan_code(const Scope& scope) {
    Timer timer("scan_code");
    walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
      editable_cfg_adapter::iterate(&code, [&](MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_NEW_INSTANCE ||
            insn->opcode() == OPCODE_CONST_CLASS) {
          // occurrences of "const-class" doesn't actually mean that the class
          // can be instantiated, but since it's then possible via reflection,
          // we treat it as such
          m_instantiated_types.insert(insn->get_type());
        }
        if (insn->opcode() == OPCODE_INVOKE_SUPER) {
          auto callee_ref = insn->get_method();
          auto callee = resolve_method(callee_ref, MethodSearch::Super, method);
          if (callee == nullptr) {
            m_unresolved_super_invoked_virtual_scopes.insert(
                VirtualScopeId::make(callee_ref));
          } else {
            m_resolved_super_invoked_methods.insert(callee);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
    });
  }

  bool is_instantiated(DexType* t) const {
    auto cls = type_class(t);
    return is_native(cls) || root(cls) || !can_rename(cls) ||
           m_instantiated_types.count(t);
  }

 public:
  OverriddenVirtualScopesAnalysis(
      const Scope& scope,
      const std::unordered_set<DexType*>& scoped_uninstantiable_types,
      const std::unordered_map<DexType*, std::unordered_set<DexType*>>&
          instantiable_children)
      : m_scoped_uninstantiable_types(scoped_uninstantiable_types) {
    Timer timer("OverriddenVirtualScopesAnalysis");

    scan_code(scope);

    ConcurrentMap<const DexType*, VirtualScopeIdSet> defined_virtual_scopes;
    walk::parallel::classes(scope, [&](DexClass* cls) {
      VirtualScopeIdSet virtual_scopes;
      for (auto method : cls->get_vmethods()) {
        VirtualScopeId virtual_scope = VirtualScopeId::make(method);
        virtual_scopes.emplace(virtual_scope);
      }
      defined_virtual_scopes.emplace(cls->get_type(),
                                     std::move(virtual_scopes));
    });

    for (auto cls : scope) {
      compute_transitively_defined_virtual_scope(
          instantiable_children, defined_virtual_scopes, cls->get_type());
    }
  }

  bool keep_code(DexMethod* method) const {
    if (is_static(method)) {
      return true;
    }
    if (m_scoped_uninstantiable_types.count(method->get_class())) {
      return false;
    }
    if (!method->is_virtual()) {
      return true;
    }
    if (m_resolved_super_invoked_methods.count(method) ||
        m_unresolved_super_invoked_virtual_scopes.count(
            VirtualScopeId::make(method))) {
      return true;
    }
    if (is_instantiated(method->get_class())) {
      return true;
    }
    VirtualScopeId virtual_scope = VirtualScopeId::make(method);
    return !m_transitively_defined_virtual_scopes.at(method->get_class())
                .count(virtual_scope);
  }
};

} // namespace

RemoveUninstantiablesPass::Stats& RemoveUninstantiablesPass::Stats::operator+=(
    const Stats& that) {
  this->instance_ofs += that.instance_ofs;
  this->invokes += that.invokes;
  this->field_accesses_on_uninstantiable +=
      that.field_accesses_on_uninstantiable;
  this->throw_null_methods += that.throw_null_methods;
  this->abstracted_classes += that.abstracted_classes;
  this->abstracted_vmethods += that.abstracted_vmethods;
  this->removed_vmethods += that.removed_vmethods;
  this->get_uninstantiables += that.get_uninstantiables;
  this->check_casts += that.check_casts;
  return *this;
}

RemoveUninstantiablesPass::Stats RemoveUninstantiablesPass::Stats::operator+(
    const Stats& that) const {
  auto copy = *this;
  copy += that;
  return copy;
}

void RemoveUninstantiablesPass::Stats::report(PassManager& mgr) const {
#define REPORT(STAT)                                                           \
  do {                                                                         \
    mgr.incr_metric(#STAT, STAT);                                              \
    TRACE(                                                                     \
        RMUNINST, 2, "  " #STAT ": %d/%" PRId64, STAT, mgr.get_metric(#STAT)); \
  } while (0)

  TRACE(RMUNINST, 2, "RemoveUninstantiablesPass Stats:");

  REPORT(instance_ofs);
  REPORT(invokes);
  REPORT(field_accesses_on_uninstantiable);
  REPORT(throw_null_methods);
  REPORT(abstracted_classes);
  REPORT(abstracted_vmethods);
  REPORT(removed_vmethods);
  REPORT(get_uninstantiables);
  REPORT(check_casts);

#undef REPORT
}

// Map deobfuscated type name in internal format (La/b/MyClass;) to the obfuscated DexType*.
std::unordered_map<std::string, DexType*> make_deobfuscated_map(const std::unordered_set<DexType*>& obfuscated_types) {
  std::unordered_map<std::string, DexType*> deobfuscated_to_type;
  for (auto obfuscated_type : obfuscated_types) {
    auto obfuscated_cls = type_class(obfuscated_type);
    auto deobfuscated_name = obfuscated_cls->get_deobfuscated_name().str();
    TRACE(
      RMUNINST, 4, "Mapping deobfuscated name to obfuscated type: '%s' -> '%s'",
      deobfuscated_name.c_str(), SHOW(obfuscated_type));
    if (deobfuscated_to_type.count(deobfuscated_name) > 0) {
      always_assert_log(
        deobfuscated_to_type.count(deobfuscated_name),
        "Multiple uninstantiable types with same deobfuscated name! '%s' == '%s' && '%s'",
        deobfuscated_name.c_str(),
        SHOW(obfuscated_type),
        SHOW(deobfuscated_to_type.at(deobfuscated_name))
      );
    }
    deobfuscated_to_type.emplace(deobfuscated_name, obfuscated_type);
  }
  return deobfuscated_to_type;
}

// Handles a single line of usage.txt proguard file from `-printusage`.
// Focuses on looking for removed constructors for instantiability.
//
// Typical usage.txt contents:
// ----
// androidx.core.view.ViewKt$postOnAnimationDelayed$runnable$1
// androidx.core.view.ViewParentCompat:
//     private static int[] sTempNestedScrollConsumed
//     41:41:private ViewParentCompat()
// ----
// A classname on its own line means the whole class was removed and can be
// ignored.
// If the classname is followed by a ":", then what follows is a 4-space
// indented list of parts that were removed. The list continues until we find a
// de-indent which means we once again have a class name.
// We specifically look for removed constructors by doing a name check for a
// removed method that is named the name of the class. All names are
// unobfuscated (pre-proguard) and need to be obfuscated to find relevant redex
// classes.
void UsageHandler::handle_usage_line(
    const std::string& line,
    const std::unordered_map<std::string, DexType*>& deobfuscated_to_uninstantiable_type,
    std::unordered_set<DexType*>& uninstantiable_types) {
  TRACE(RMUNINST, 5, "usage.txt line: %s", line.c_str());
  // The current line is a class
  if (line.find(" ") != 0) {
    int dot_index = line.find_last_of('.');
    int end_index = line.find_first_of(':');
    cls_name = line.substr(dot_index + 1, end_index - dot_index - 1);
    // usage lines use external naming (a.b.MyClass) and we need to translate
    // to internal naming (La/b/MyClass;) to match type names
    type_name = convert_type(line.substr(0, end_index));
    usage_type = DexType::get_type(type_name);

    TRACE(RMUNINST, 5, "usage.txt line type (deobfuscated) name: '%s'", type_name.c_str());
    if (usage_type != nullptr) {
      TRACE(RMUNINST, 5, "usage.txt line type (deobfuscated): '%s'", SHOW(usage_type));
    } else {
      TRACE(RMUNINST, 5, "usage.txt line type (deobfuscated) does not exist!");
    }

    if (deobfuscated_to_uninstantiable_type.count(type_name) <= 0) {
      TRACE(RMUNINST, 5, "usage.txt type has no obfuscated version: %s", type_name.c_str());
      cls_type = usage_type;
    } else {
      cls_type = deobfuscated_to_uninstantiable_type.at(type_name);
      TRACE(RMUNINST, 5, "usage.txt obfuscated type: '%s' -> '%s'", SHOW(cls_type), type_name.c_str());
    }
    if (uninstantiable_types.count(cls_type)) {
      has_ctor = false;
    } else {
      has_ctor = true;
    }
  } else {
    if (has_ctor) {
      return;
    }
    // The current line is a constructor method
    if (line.find(cls_name + "(") != std::string::npos) {
      has_ctor = true;
      if (uninstantiable_types.count(cls_type)) {
        TRACE(
          RMUNINST, 4,
          "Keeping class due to proguard-removed constructor: %s",
          SHOW(cls_type));
        count++;
      } else {
        TRACE(
          RMUNINST, 4, "Keep class not present! Skipping: %s",
          SHOW(cls_type));
      }
      uninstantiable_types.erase(cls_type);
    }
  }
}

// Prune the list of possibly uninstantiable types by looking at the <init>
// constructor that Proguard deleted.
void RemoveUninstantiablesPass::readUsage(std::istream& usage_file, std::unordered_set<DexType*>& uninstantiable_types) {
  // uninstantiable_types may have obfuscated names. Deobfuscate for usage.txt.
  std::unordered_map<std::string, DexType*> deobfuscated_to_uninstantiable_type =
      make_deobfuscated_map(uninstantiable_types);
  TRACE(
    RMUNINST, 1, "Mapped %lu deobfuscated<->obfuscated types",
    deobfuscated_to_uninstantiable_type.size());

  UsageHandler uh;
  std::string line;
  while(getline(usage_file, line)) {
    uh.handle_usage_line(line, deobfuscated_to_uninstantiable_type, uninstantiable_types);
  }
  TRACE(
    RMUNINST, 1,
    "Removed %u types from uninstantiable_types by reading proguard file",
    uh.count);
}

// Computes set of uninstantiable types, also looking at the type system to
// find non-external (and non-native)...
// - interfaces that are not annotations, are not root (or unrenameable) and
//   do not contain root (or unrenameable) methods and have no non-abstract
//   classes implementing them, and
// - abstract (non-interface) classes that are not extended by any non-abstract
//   class
std::unordered_set<DexType*>
RemoveUninstantiablesPass::compute_scoped_uninstantiable_types(
    const Scope& scope,
    std::unordered_map<DexType*, std::unordered_set<DexType*>>*
        instantiable_children) {
  // First, we compute types that might possibly be uninstantiable, and classes
  // that we consider instantiable.
  std::unordered_set<DexType*> uninstantiable_types;
  std::unordered_set<const DexClass*> instantiable_classes;
  auto is_interface_instantiable = [](const DexClass* interface) {
    if (is_annotation(interface) || interface->is_external() ||
        is_native(interface) || root(interface) || !can_rename(interface)) {
      return true;
    }
    for (auto method : interface->get_vmethods()) {
      if (root(method) || !can_rename(method)) {
        return true;
      }
    }
    return false;
  };
  walk::classes(scope, [&](const DexClass* cls) {
    if (type::is_uninstantiable_class(cls->get_type())) {
      uninstantiable_types.insert(cls->get_type());
    } else if (is_interface(cls) && !is_interface_instantiable(cls)) {
      uninstantiable_types.insert(cls->get_type());
    } else {
      instantiable_classes.insert(cls);
    }
  });

  if(RemoveUninstantiablesPass::m_proguard_usage_name.compare("") != 0) {
    std::ifstream infile;
    infile.open(RemoveUninstantiablesPass::m_proguard_usage_name.data());
    always_assert(infile.is_open());
    readUsage(infile, uninstantiable_types);
    infile.close();
  } else if (RemoveUninstantiablesPass::test_only_usage_file_input != nullptr) {
    readUsage(*RemoveUninstantiablesPass::test_only_usage_file_input, uninstantiable_types);
  }

  // Next, we prune the list of possibly uninstantiable types by looking at
  // what instantiable classes implement and extend.
  std::unordered_set<const DexClass*> visited;
  std::function<void(const DexClass*)> visit;
  visit = [&](const DexClass* cls) {
    if (cls == nullptr || !visited.insert(cls).second) {
      return;
    }
    if (instantiable_children) {
      (*instantiable_children)[cls->get_super_class()].insert(cls->get_type());
    }
    uninstantiable_types.erase(cls->get_type());
    for (auto interface : *cls->get_interfaces()) {
      visit(type_class(interface));
    }
    visit(type_class(cls->get_super_class()));
  };

  size_t count = uninstantiable_types.size();
  for (auto cls : instantiable_classes) {
    visit(cls);
  }

  TRACE(
    RMUNINST, 1, "Removed %lu classes by pruning parents/interfaces",
    count - uninstantiable_types.size());
  uninstantiable_types.insert(type::java_lang_Void());
  return uninstantiable_types;
}

RemoveUninstantiablesPass::Stats
RemoveUninstantiablesPass::replace_uninstantiable_refs(
    const std::unordered_set<DexType*>& scoped_uninstantiable_types,
    cfg::ControlFlowGraph& cfg) {
  cfg::CFGMutation m(cfg);

  Stats stats;
  auto ii = InstructionIterable(cfg);
  npe::NullPointerExceptionCreator npe_creator(&cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    auto op = insn->opcode();
    switch (op) {
    case OPCODE_INSTANCE_OF:
      if (scoped_uninstantiable_types.count(insn->get_type())) {
        auto dest = cfg.move_result_of(it)->insn->dest();
        m.replace(it, {ir_const(dest, 0)});
        stats.instance_ofs++;
      }
      continue;

    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_SUPER:
      // Note that we don't want to call resolve_method here: The most precise
      // class information is already present in the supplied method reference,
      // which gives us the best change of finding an uninstantiable type.
      if (scoped_uninstantiable_types.count(insn->get_method()->get_class())) {
        m.replace(it, npe_creator.get_insns(insn));
        stats.invokes++;
      }
      continue;

    case OPCODE_CHECK_CAST:
      if (scoped_uninstantiable_types.count(insn->get_type())) {
        auto src = insn->src(0);
        auto dest = cfg.move_result_of(it)->insn->dest();
        m.replace(it,
                  {ir_check_cast(src, type::java_lang_Void()),
                   ir_move_result_pseudo_object(dest), ir_const(src, 0),
                   ir_const(dest, 0)});
        stats.check_casts++;
      }
      continue;

    default:
      break;
    }

    if (opcode::is_an_iget(op) &&
        scoped_uninstantiable_types.count(insn->get_field()->get_class())) {
      m.replace(it, npe_creator.get_insns(insn));
      stats.field_accesses_on_uninstantiable++;
      continue;
    }

    if (opcode::is_an_iput(op) &&
        scoped_uninstantiable_types.count(insn->get_field()->get_class())) {
      m.replace(it, npe_creator.get_insns(insn));
      stats.field_accesses_on_uninstantiable++;
      continue;
    }

    if ((opcode::is_an_iget(op) || opcode::is_an_sget(op)) &&
        scoped_uninstantiable_types.count(insn->get_field()->get_type())) {
      auto dest = cfg.move_result_of(it)->insn->dest();
      m.replace(it, {ir_const(dest, 0)});
      stats.get_uninstantiables++;
      continue;
    }
  }

  m.flush();
  return stats;
}

RemoveUninstantiablesPass::Stats
RemoveUninstantiablesPass::replace_all_with_throw(cfg::ControlFlowGraph& cfg) {
  auto* entry = cfg.entry_block();
  always_assert_log(entry, "Expect an entry block");

  auto it = entry->to_cfg_instruction_iterator(
      entry->get_first_non_param_loading_insn());
  always_assert_log(!it.is_end(), "Expecting a non-param loading instruction");

  auto tmp = cfg.allocate_temp();
  cfg.insert_before(it, {ir_const(tmp, 0), ir_throw(tmp)});

  Stats stats;
  stats.throw_null_methods++;
  return stats;
}

void RemoveUninstantiablesPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  std::unordered_map<DexType*, std::unordered_set<DexType*>>
      instantiable_children;
  std::unordered_set<DexType*> scoped_uninstantiable_types =
      compute_scoped_uninstantiable_types(scope, &instantiable_children);
  TRACE(RMUNINST, 2, "Total instantiable types: %lu", instantiable_children.size());
  TRACE(RMUNINST, 2, "Total uninstantiable types: %lu", scoped_uninstantiable_types.size());
  for (auto utype : scoped_uninstantiable_types) {
    TRACE(
      RMUNINST, 5, "uninstantiable class: %s -> %s",
      SHOW(utype), type_class(utype)->get_deobfuscated_name_or_empty().c_str());
  }
  for (auto itype : instantiable_children) {
    if (itype.first == nullptr) {
      continue;
    }
    TRACE(
      RMUNINST, 5, "instantible class: %s -> %s",
      SHOW(itype.first), type_class(itype.first)->get_deobfuscated_name_or_empty().c_str());
  }
  OverriddenVirtualScopesAnalysis overridden_virtual_scopes_analysis(
      scope, scoped_uninstantiable_types, instantiable_children);
  // We perform structural changes, i.e. whether a method has a body and
  // removal, as a post-processing step, to streamline the main operations
  struct ClassPostProcessing {
    std::unordered_map<DexMethod*, DexMethod*> remove_vmethods;
    std::unordered_set<DexMethod*> abstract_vmethods;
  };
  ConcurrentMap<DexClass*, ClassPostProcessing> class_post_processing;
  Stats stats = walk::parallel::methods<Stats>(
      scope,
      [&scoped_uninstantiable_types,
       &overridden_virtual_scopes_analysis,
       &class_post_processing](DexMethod* method) -> Stats {
        Stats stats;

        auto code = method->get_code();
        if (method->rstate.no_optimizations() || code == nullptr) {
          return stats;
        }

        code->build_cfg();
        if (overridden_virtual_scopes_analysis.keep_code(method)) {
          stats += replace_uninstantiable_refs(scoped_uninstantiable_types,
                                               code->cfg());
        } else {
          auto overridden_method =
              method->is_virtual()
                  ? resolve_method(method, MethodSearch::Super, method)
                  : nullptr;
          if (overridden_method == nullptr && method->is_virtual()) {
            class_post_processing.update(
                type_class(method->get_class()),
                [method](DexClass*, ClassPostProcessing& cpp, bool) {
                  cpp.abstract_vmethods.insert(method);
                });
            stats.abstracted_vmethods++;
          } else if (overridden_method != nullptr && can_delete(method) &&
                     (is_protected(method) || is_public(overridden_method))) {
            always_assert(overridden_method != method);
            class_post_processing.update(
                type_class(method->get_class()),
                [method,
                 overridden_method](DexClass*, ClassPostProcessing& cpp, bool) {
                  cpp.remove_vmethods.emplace(method, overridden_method);
                });
            stats.removed_vmethods++;
          } else {
            stats += replace_all_with_throw(code->cfg());
          }
        }
        code->clear_cfg();
        return stats;
      });

  // Post-processing:
  // 1. make methods abstract (stretty straightforward), and
  // 2. remove methods (per class in parallel for best performance, and rewrite
  // all invocation references)
  std::vector<DexClass*> classes_with_removed_vmethods;
  std::unordered_map<DexMethodRef*, DexMethodRef*> removed_vmethods;
  for (auto& p : class_post_processing) {
    auto cls = p.first;
    auto& cpp = p.second;
    if (!cpp.abstract_vmethods.empty()) {
      if (!is_abstract(cls)) {
        stats.abstracted_classes++;
        cls->set_access((cls->get_access() & ~ACC_FINAL) | ACC_ABSTRACT);
      }
      for (auto method : cpp.abstract_vmethods) {
        method->set_access(
            (DexAccessFlags)((method->get_access() & ~ACC_FINAL) |
                             ACC_ABSTRACT));
        method->set_code(nullptr);
      }
    }
    if (!cpp.remove_vmethods.empty()) {
      classes_with_removed_vmethods.push_back(cls);
      removed_vmethods.insert(cpp.remove_vmethods.begin(),
                              cpp.remove_vmethods.end());
    }
  }

  walk::parallel::classes(classes_with_removed_vmethods,
                          [&class_post_processing](DexClass* cls) {
                            auto& cpp = class_post_processing.at_unsafe(cls);
                            for (auto& p : cpp.remove_vmethods) {
                              cls->remove_method(p.first);
                              DexMethod::erase_method(p.first);
                              DexMethod::delete_method(p.first);
                            }
                          });

  // Forward chains.
  method_dedup::fixup_references_to_removed_methods(scope, removed_vmethods);

  stats.report(mgr);
}

static RemoveUninstantiablesPass s_pass;
