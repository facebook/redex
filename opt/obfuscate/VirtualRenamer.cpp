/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VirtualRenamer.h"

#include <map>
#include <set>

#include "ConcurrentContainers.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ObfuscateUtils.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "VirtualScope.h"
#include "Walkers.h"

using namespace virt_scope;

namespace {

/**
 * Debug utility to collect info about ClassScopes.
 * Likely to disappear soon....
 */
void scope_info(const ClassScopes& class_scopes) {
  std::map<int, int> easy_scopes;
  std::map<int, int> impl_scopes;
  std::map<int, int> cant_rename_scopes;

  class_scopes.walk_virtual_scopes(
      [&](const DexType* type, const VirtualScope* scope) {
        const auto cls = type_class(type);
        if (cls == nullptr || cls->is_external()) return;
        auto scope_meth_count = scope->methods.size();
        if (scope_meth_count > 100) {
          TRACE(OBFUSCATE, 2, "BIG SCOPE: %zu on %s", scope_meth_count,
                SHOW(scope->methods[0].first));
        }
        // class is internal
        if (!can_rename_scope(scope)) {
          cant_rename_scopes[scope_meth_count]++;
          return;
        }
        if (is_impl_scope(scope)) {
          impl_scopes[scope_meth_count]++;
          return;
        }
        easy_scopes[scope_meth_count]++;
      });

  const auto scope_count = [](const std::map<int, int>& map) {
    size_t c = 0;
    for (const auto& it : map) {
      c += it.second;
    }
    return c;
  };
  const auto method_count = [](const std::map<int, int>& map) {
    size_t c = 0;
    for (const auto& it : map) {
      c += (it.first * it.second);
    }
    return c;
  };
  TRACE(OBFUSCATE, 2,
        "scopes (scope count, method count)"
        "easy (%zu, %zu), "
        "impl (%zu, %zu), "
        "can't rename (%zu, %zu)\n",
        scope_count(easy_scopes), method_count(easy_scopes),
        scope_count(impl_scopes), method_count(impl_scopes),
        scope_count(cant_rename_scopes), method_count(cant_rename_scopes));

  const auto by_meth_count = [](const char* msg, std::map<int, int> map) {
    TRACE(OBFUSCATE, 2, "%s", msg);
    for (auto it = map.rbegin(); it != map.rend(); ++it) {
      TRACE(OBFUSCATE, 2, "%d <= %d", it->first, it->second);
    }
  };
  TRACE(OBFUSCATE, 2,
        "method count in scope <= scope count with that method count");
  by_meth_count("EasyScopes:", easy_scopes);
  by_meth_count("ImplScopes:", impl_scopes);
  by_meth_count("CantRenameScopes:", cant_rename_scopes);
}

/**
 * Find a method with the given (name, proto) in a class.
 */
DexMethod* find_method(const DexClass* cls,
                       const DexString* name,
                       const DexProto* proto) {
  for (auto& vmeth : cls->get_vmethods()) {
    if (vmeth->get_name() == name && vmeth->get_proto() == proto) {
      return vmeth;
    }
  }
  return (DexMethod*)nullptr;
};

// keep a map from defs to all refs resolving to that def
using RefsMap =
    std::unordered_map<DexMethod*,
                       std::set<DexMethodRef*, dexmethods_comparator>>;

// Uncomment and use this as a prefix for virtual method
// names for debugging
// const std::string prefix = __Redex__";
const std::string prefix;

const DexString* get_name(int seed) {
  std::string name;
  obfuscate_utils::compute_identifier(seed, &name);
  if (!prefix.empty()) {
    name = prefix + name;
  }
  return DexString::make_string(name);
}

struct VirtualRenamer {
  VirtualRenamer(
      const ClassScopes& class_scopes,
      const RefsMap& def_refs,
      std::unordered_map<std::string, uint32_t>* elms,
      std::unordered_map<const DexType*, std::string>* cache,
      const std::unordered_map<const DexClass*, int>& next_dmethod_seeds)
      : class_scopes(class_scopes),
        def_refs(def_refs),
        stack_trace_elements(elms),
        external_name_cache(cache),
        next_dmethod_seeds(next_dmethod_seeds) {}

  int rename_virtual_scopes(const DexType* type, int& seed);
  int rename_interface_scopes(int& seed);

 private:
  const ClassScopes& class_scopes;
  const RefsMap& def_refs;
  // When avoid_stack_trace_collision is true this is used to keep a ref count
  // of a given fully qualified method name (sans parameters); i.e. the line
  // that will be printed when the method appears in a stack trace (internally
  // in ART this is called a stack trace element). As methods are renamed
  // their ref counts get updated, and if the ref count drops to 0 then its
  // entry is erased. When avoid_stack_trace_collision is false then this is
  // null and collision avoidance is disabled.
  std::unordered_map<std::string, uint32_t>* stack_trace_elements;
  // Note these entries contain trailing periods
  std::unordered_map<const DexType*, std::string>* external_name_cache;
  const std::unordered_map<const DexClass*, int>& next_dmethod_seeds;
  mutable std::unordered_map<const VirtualScope*, int> next_virtualscope_seeds;
  mutable std::unordered_map<const DexType*, TypeSet> hier_cache;

  const std::string& get_prefix(const DexType* type) const {
    always_assert(external_name_cache != nullptr);
    auto iter = external_name_cache->find(type);
    always_assert(iter != external_name_cache->end());
    return iter->second;
  }

  // Retrieves the next seed that won't overlap with dmethods, considering all
  // classes participating in the given virtual scope
  int get_next_virtualscope_seeds(const VirtualScope* scope) const {
    auto it = next_virtualscope_seeds.find(scope);
    if (it != next_virtualscope_seeds.end()) {
      return it->second;
    }
    int seed = 0;
    for (auto& m : scope->methods) {
      auto it2 = next_dmethod_seeds.find(type_class(m.first->get_class()));
      if (it2 != next_dmethod_seeds.end()) {
        seed = std::max(seed, it2->second);
      }
    }
    next_virtualscope_seeds.emplace(scope, seed);
    return seed;
  }

  void rename(DexMethodRef* meth, const DexString* name);
  int rename_scope_ref(DexMethod* meth, const DexString* name);
  int rename_scope(const VirtualScope* scope, const DexString* name);

  const DexString* get_unescaped_name(
      const std::vector<const VirtualScope*>& scopes, int& seed) const;
  const DexString* get_unescaped_name(const VirtualScope* scope,
                                      int& seed) const;
  bool usable_name(const DexString* name, const VirtualScope* scope) const;
};

/**
 * Rename a given method with the given name.
 */
void VirtualRenamer::rename(DexMethodRef* meth, const DexString* name) {
  // redex_assert(meth->is_concrete() && !meth->is_external());
  DexMethodSpec spec;
  spec.cls = meth->get_class();
  spec.name = name;
  spec.proto = meth->get_proto();
  if (stack_trace_elements) {
    std::string ste = get_prefix(meth->get_class()) + meth->str();
    auto iter = stack_trace_elements->find(ste);
    // We don't find this ste if it's a miranda method
    if (iter != stack_trace_elements->end()) {
      // We've found this ste, so let's decrement its ref count, and if it
      // reaches 0 then remove it so we don't have any empty entries
      iter->second -= 1;
      if (iter->second == 0) {
        stack_trace_elements->erase(iter);
      }
    }
  }
  meth->change(spec, false /* rename on collision */);

  if (stack_trace_elements) {
    std::string ste = get_prefix(meth->get_class()) + name->str();
    auto res = stack_trace_elements->emplace(std::move(ste), 1);
    // Ideally we've picked a new name that doesn't collide with any other
    // method, so this assert should never fire. We leave this here in case
    // my human brain foobarred the logic (or in a refactor some other
    // assumption e.g. thread safety are changed)
    always_assert(res.second);
  }
}

/**
 * Rename all refs to the given method.
 */
int VirtualRenamer::rename_scope_ref(DexMethod* meth, const DexString* name) {
  int renamed = 0;
  const auto& refs = def_refs.find(meth);
  if (refs == def_refs.end()) return renamed;
  for (auto& ref : refs->second) {
    rename(ref, name);
    renamed++;
  }
  return renamed;
}

/**
 * Rename an entire virtual scope.
 */
int VirtualRenamer::rename_scope(const VirtualScope* scope,
                                 const DexString* name) {
  int renamed = 0;
  for (auto& vmeth : scope->methods) {
    rename(vmeth.first, name);
    if (vmeth.first->is_concrete())
      renamed++;
    else {
      TRACE(OBFUSCATE, 2, "not concrete %s", SHOW(vmeth.first));
    }
  }
  redex_assert(!scope->methods.empty());
  rename_scope_ref(scope->methods[0].first, name);
  return renamed;
}

/**
 * A name is usable if it does not collide with an existing
 * one in the def and ref space.
 */
bool VirtualRenamer::usable_name(const DexString* name,
                                 const VirtualScope* scope) const {
  const auto root = scope->type;
  auto it = hier_cache.find(root);
  if (it == hier_cache.end()) {
    auto hier = get_all_children(class_scopes.get_class_hierarchy(), root);
    hier.insert(root);
    it = hier_cache.emplace(root, std::move(hier)).first;
  }
  const auto& hier = it->second;
  const auto proto = scope->methods[0].first->get_proto();
  bool has_ste = stack_trace_elements != nullptr;
  for (const auto& type : hier) {
    if (DexMethod::get_method(const_cast<DexType*>(type), name, proto) !=
        nullptr) {
      return false;
    }
    if (has_ste) {
      auto ste = get_prefix(type) + name->str();
      if (stack_trace_elements->find(ste) != stack_trace_elements->end()) {
        return false;
      }
    }
  }
  return true;
}

/*
 * Operate on 'seed' and find a name for scope that does not
 * lead to any collision for all defs or refs.
 * * Update 'seed' *
 */
const DexString* VirtualRenamer::get_unescaped_name(const VirtualScope* scope,
                                                    int& seed) const {
  seed = std::max(seed, get_next_virtualscope_seeds(scope));
  auto name = get_name(seed++);
  while (!usable_name(name, scope)) {
    name = get_name(seed++);
  }
  return name;
}

/*
 * Operate on 'seed' and find a name for all scopes that does not
 * lead to any collision for all defs or refs.
 * * Update 'seed' *
 */
const DexString* VirtualRenamer::get_unescaped_name(
    const std::vector<const VirtualScope*>& scopes, int& seed) const {
  // advance seed as necessary, skipping over dmethods
  for (const auto& scope : scopes) {
    seed = std::max(seed, get_next_virtualscope_seeds(scope));
  }
  while (true) {
    auto name = get_name(seed++);
    auto is_usable = [&]() {
      for (const auto& scope : scopes) {
        if (!usable_name(name, scope)) return false;
      }
      return true;
    }();
    if (!is_usable) {
      continue;
    }
    return name;
  }
}

int VirtualRenamer::rename_interface_scopes(int& seed) {
  int renamed = 0;
  class_scopes.walk_all_intf_scopes(
      [&](const DexString* name,
          const DexProto* proto,
          const std::vector<const VirtualScope*>& scopes,
          const TypeSet& intfs) {
        // if any scope cannot be renamed let it go, we don't
        // rename anything
        TRACE(OBFUSCATE, 5, "Got %zu scopes for %s%s", scopes.size(),
              SHOW(name), SHOW(proto));
        for (auto& scope : scopes) {
          redex_assert(type_class(scope->type) != nullptr);
          if (type_class(scope->type)->is_external()) {
            TRACE(OBFUSCATE, 5, "External impl scope %s",
                  SHOW(scope->methods[0].first));
            return;
          }
          if (!can_rename_scope(scope)) {
            TRACE(OBFUSCATE, 5, "Cannot rename impl scope %s",
                  SHOW(scope->methods[0].first));
            return;
          }
        }
        for (const auto& intf : intfs) {
          const auto& intf_cls = type_class(intf);
          if (intf_cls == nullptr) {
            TRACE(OBFUSCATE, 5, "No interface class %s", SHOW(intf));
            return;
          }
          if (intf_cls->is_external()) {
            TRACE(OBFUSCATE, 5, "External interface %s", SHOW(intf));
            return;
          }
        }
        // if any interface method that we are about to rename
        // cannot be renamed give up
        for (const auto& intf : intfs) {
          redex_assert(type_class(intf) != nullptr);
          const auto meth = find_method(type_class(intf), name, proto);
          redex_assert(meth != nullptr);
          if (!can_rename(meth)) {
            TRACE(OBFUSCATE, 5, "Cannot rename %s", SHOW(meth));
            return;
          }
        }
        // all scopes can be renamed, go for it
        auto new_name = get_unescaped_name(scopes, seed);
        TRACE(OBFUSCATE, 5, "New name %s for %s%s", SHOW(new_name), SHOW(name),
              SHOW(proto));
        for (const auto& scope : scopes) {
          renamed += rename_scope(scope, new_name);
        }
        // rename interface method only
        for (const auto& intf : intfs) {
          auto intf_cls = type_class(intf);
          redex_assert(intf_cls != nullptr);
          auto intf_meth = find_method(intf_cls, name, proto);
          always_assert_log(intf_meth != nullptr,
                            "cannot find interface method for %s%s", SHOW(name),
                            SHOW(proto));
          TRACE(OBFUSCATE, 5, "New name %s for %s", SHOW(new_name),
                SHOW(intf_meth));
          rename(intf_meth, new_name);
          rename_scope_ref(intf_meth, new_name);
          renamed++;
        }
      });
  return renamed;
}

/**
 * Rename only scopes that are not interface and can_rename.
 */
int VirtualRenamer::rename_virtual_scopes(const DexType* type, int& seed) {
  int renamed = 0;
  const auto cls = type_class(type);
  TRACE(OBFUSCATE, 5, "Attempting to rename %s", SHOW(type));
  // object or external classes are not renamable, move
  // to the children
  if (cls != nullptr && !cls->is_external()) {
    auto scopes_copy = class_scopes.get(type);
    std::sort(scopes_copy.begin(), scopes_copy.end(),
              [&](const VirtualScope* a, const VirtualScope* b) {
                // prefer scopes which have extra seed values available
                auto a_seed = std::max(seed, get_next_virtualscope_seeds(a));
                auto b_seed = std::max(seed, get_next_virtualscope_seeds(b));
                if (a_seed != b_seed) {
                  return a_seed < b_seed;
                }
                auto a_method = a->methods[0].first;
                auto b_method = b->methods[0].first;
                // then sort by scopes...
                if (a_method->get_proto() != b_method->get_proto()) {
                  return dexprotos_comparator()(a_method->get_proto(),
                                                b_method->get_proto());
                }
                // then by access...
                auto a_access =
                    a_method->is_def() ? (uint32_t)a_method->get_access() : 0;
                auto b_access =
                    b_method->is_def() ? (uint32_t)b_method->get_access() : 0;
                if (a_access != b_access) {
                  return a_access < b_access;
                }
                // otherwise, by root methods
                return dexmethods_comparator()(a_method, b_method);
              });
    // rename all scopes at this level that are not interface
    // and can be renamed
    TRACE(OBFUSCATE, 5, "Found %zu scopes in %s", scopes_copy.size(),
          SHOW(type));
    for (auto& scope : scopes_copy) {
      if (!can_rename_scope(scope)) {
        TRACE(OBFUSCATE, 5, "Cannot rename %s", SHOW(scope->methods[0].first));
        continue;
      }
      if (is_impl_scope(scope)) {
        TRACE(OBFUSCATE, 5, "Impl scope %s", SHOW(scope->methods[0].first));
        continue;
      }
      auto name = get_unescaped_name(scope, seed);
      TRACE(OBFUSCATE, 5, "New name %s for %s", SHOW(name),
            SHOW(scope->methods[0].first));
      renamed += rename_scope(scope, name);
    }
  }

  // will be used for interface renaming, effectively this
  // gets the last name (seed) for all virtual scopes and
  // interface are treated as all being at the same scope,
  // that is, they will all have different names irrespective
  // of where they are in the hierarchy
  int max_seed = seed;
  // recurse into children
  for (const auto& child :
       get_children(class_scopes.get_class_hierarchy(), type)) {
    int base_seed = seed;
    renamed += rename_virtual_scopes(child, base_seed);
    max_seed = std::max(max_seed, base_seed);
  }
  seed = max_seed;
  return renamed;
}

/**
 * Collect all method refs to concrete methods (definitions).
 */
void collect_refs(Scope& scope, RefsMap& def_refs) {
  ConcurrentMap<DexMethod*, std::set<DexMethodRef*, dexmethods_comparator>>
      concurrent_def_refs;
  walk::parallel::opcodes(
      scope, [](DexMethod*) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        if (!insn->has_method()) return;
        auto callee = insn->get_method();
        if (callee->is_concrete()) return;
        auto cls = type_class(callee->get_class());
        if (cls == nullptr || cls->is_external()) return;
        DexMethod* top = nullptr;
        if (is_interface(cls)) {
          top = resolve_method(callee, MethodSearch::Interface);
        } else {
          top = find_top_impl(cls, callee->get_name(), callee->get_proto());
          if (top == nullptr) {
            TRACE(OBFUSCATE, 2, "Possible top miranda: %s", SHOW(callee));
            // see if it's a virtual call to an interface miranda method
            top = find_top_intf_impl(cls, callee->get_name(),
                                     callee->get_proto());
            if (top != nullptr) {
              TRACE(OBFUSCATE, 2, "Top miranda: %s", SHOW(top));
            }
          }
        }
        if (top == nullptr || top == callee) return;
        redex_assert(type_class(top->get_class()) != nullptr);
        if (type_class(top->get_class())->is_external()) return;
        // it's a top definition on an internal class, save it
        concurrent_def_refs.update(
            top, [&](auto*, auto& set, bool) { set.insert(callee); });
      });
  def_refs = concurrent_def_refs.move_to_container();
}

} // namespace

/**
 * Rename virtual methods.
 */
size_t rename_virtuals(
    Scope& scope,
    bool avoid_stack_trace_collision,
    const std::unordered_map<const DexClass*, int>& next_dmethod_seeds) {
  // build a ClassScope a RefsMap and a VirtualRenamer
  ClassScopes class_scopes(scope);
  scope_info(class_scopes);
  RefsMap def_refs;
  collect_refs(scope, def_refs);
  std::unordered_map<std::string, uint32_t> stack_trace_elements;
  std::unordered_map<const DexType*, std::string> external_cache;
  if (avoid_stack_trace_collision) {
    for (const auto& cls : scope) {
      std::string pref = java_names::internal_to_external(cls->str()) + ".";
      auto meths_visitor = [&](const std::vector<DexMethod*>& methods) {
        for (const DexMethod* method : methods) {
          std::string ste = pref + method->str();
          // We're 100% ok with the default construction of an entry here, since
          // after this line that would give said entry the correct ref count
          // of 1.
          stack_trace_elements[std::move(ste)] += 1;
        }
      };
      meths_visitor(cls->get_dmethods());
      meths_visitor(cls->get_vmethods());
      auto emp_res = external_cache.emplace(cls->get_type(), std::move(pref));
      always_assert(emp_res.second);
    }
  }
  VirtualRenamer vr(class_scopes,
                    def_refs,
                    avoid_stack_trace_collision ? &stack_trace_elements
                                                : nullptr,
                    avoid_stack_trace_collision ? &external_cache : nullptr,
                    next_dmethod_seeds);

  // rename virtual only first
  const auto obj_t = type::java_lang_Object();
  int seed = 0;
  size_t renamed = vr.rename_virtual_scopes(obj_t, seed);
  TRACE(OBFUSCATE, 2, "Virtual renamed: %zu", renamed);

  // rename interfaces
  std::unordered_set<const VirtualScope*> visited;
  size_t intf_renamed = vr.rename_interface_scopes(seed);
  TRACE(OBFUSCATE, 2, "Interface renamed: %zu", intf_renamed);
  TRACE(OBFUSCATE, 2, "MAX seed: %d", seed);
  return renamed + intf_renamed;
}
