/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "StaticRelo.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "DexClass.h"
#include "DexDebugOpcode.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Match.h"
#include "walkers.h"

namespace {

// Counters for this optimization
static int s_cls_delete_count;
static int s_meth_delete_count;
static int s_meth_move_count;
static int s_meth_could_not_move_count;
static float s_avg_relocation_load;
static int s_max_relocation_load;
static int s_single_ref_total_count;
static int s_single_ref_moved_count;

/** map of dmethod or class (T) -> method/opcode referencing dmethod or class */
template <typename T>
using refs_t = std::unordered_map<
  const T*, std::vector<std::pair<const DexMethod*, DexOpcode*> > >;

struct compare_dexclasses {
  bool operator()(const DexClass* a, const DexClass* b) const {
    return compare_dextypes(a->get_type(), b->get_type());
  }
};
/** all relocation candidate classes */
using candidates_t = std::set<DexClass*, compare_dexclasses>;

/**
 * Helper to visit all classes which match the given criteria.
 *
 * @param scope all classes we're processing
 * @param p A match_t<DexClass, ...> built up by m::* routines
 * @param v Visitor function
 */
template<typename P, typename V = void(DexClass*)>
void visit_classes(
  const Scope& scope, const m::match_t<DexClass, P>& p, const V& v) {
  for (const auto& cls : scope) {
    if (p.matches(cls)) {
      v(cls);
    }
  }
}

/**
 * Helper to visit all opcodes which match the given criteria.
 *
 * @param scope all classes we're processing
 * @param p A match_t<DexOpcode, ...> built up by m::* routines
 * @param v Visitor function
 */
template<typename P, typename V = void(DexMethod*, DexOpcode*)>
void visit_opcodes(
  const Scope& scope, const m::match_t<DexOpcode, P>& p, const V& v) {
  walk_opcodes(
    scope,
    [](const DexMethod*) { return true; },
    [&](const DexMethod* m, DexOpcode* opcode) {
      if (p.matches(opcode)) {
        v(m, opcode);
      }
    });
}

/**
 * Helper to build a map of DexClass* -> dex index
 *
 * @param dexen All classes in the dexen
 * @return Unordered map of DexClass* -> dex index
 */
std::unordered_map<const DexClass*, int> build_class_to_dex_map(
  const DexClassesVector& dexen) {
  std::unordered_map<const DexClass*, int> map;
  for (int i = 0, N = dexen.size() ; i < N ; ++i) {
    for (const auto& cls : dexen[i]) {
      map[cls] = i;
    }
  }
  return map;
}

/**
 * Helper to build a map of DexClass* -> order it appears in cold start list.
 *
 * @param dexen All classes in the dexen
 * @param pgo Our PGO input files
 * @return Unordered map of DexClass* -> cold start class load rank.
 *         Lower rank is earlier in cold start class load.
 */
std::unordered_map<const DexClass*, int> build_class_to_pgo_order_map(
  const DexClassesVector& dexen,
  PgoFiles& pgo) {
  auto interdex_list = pgo.get_coldstart_classes();
  std::unordered_map<std::string, DexClass*> class_string_map;
  std::unordered_map<const DexClass*, int> coldstart_classes;
  for (auto const& dex : dexen) {
    for (auto const& cls : dex) {
      class_string_map[std::string(cls->get_type()->get_name()->c_str())] = cls;
    }
  }
  int rank = 0;
  for (auto const& class_string : interdex_list) {
    if (class_string_map.count(class_string)) {
      coldstart_classes[class_string_map[class_string]] = rank++;
    }
  }
  return coldstart_classes;
}

/**
 * Helper function that scans all the bytecode in the application and
 * builds up two maps. Map goes from method/class to vector of its refs.
 *
 * @param scope all classes we're processing
 * @param dmethod_refs [out] all refs to dmethods in the application
 * @param class_refs [out] all refs to classes in the application
 *
 */
void build_refs(
  const Scope& scope,
  refs_t<DexMethod>& dmethod_refs,
  refs_t<DexClass>& class_refs) {
  // Looking for direct/static invokes or class refs
  auto match =
    m::invoke_static<DexOpcode>()
    or m::invoke_direct<DexOpcode>()
    or m::has_types<DexOpcode>();
  visit_opcodes(scope, match, [&](const DexMethod* meth, DexOpcode* opc){
    if (opc->has_types()) {
      const auto top = static_cast<DexOpcodeType*>(opc);
      const auto tref = type_class(top->get_type());
      if (tref) class_refs[tref].push_back(std::make_pair(meth, opc));
    } else {
      const auto mop = static_cast<DexOpcodeMethod*>(opc);
      const auto mref = mop->get_method();
      dmethod_refs[mref].push_back(std::make_pair(meth, opc));
    }
  });
}

/**
 * Builds a set of classes which are candidates for having their static
 * methods relocated.
 *
 * @param scope all classes we're processing
 * @param class_refs class refs for the application
 * @param dont_optimize_annos set of anno types which, if present on a method,
 *                            should prevent use from relocating that method.
 * @return an unordered set of DexClass which are static relocation candidates
 */
candidates_t build_candidates(
  const Scope& scope,
  const refs_t<DexClass>& class_refs,
  const std::unordered_set<DexType*>& dont_optimize_annos) {
  candidates_t candidates;

  auto match =
    // N.B. For now, we cheat by only finding final classes. This lets us
    // ensure that we only need to fix up references for this exact class
    // versus any of its derivatives. Although in theory ReBindRefs should
    // be making this guarantee for us?
    m::is_final<DexClass>()
    && !m::is_external<DexClass>()
    && !m::is_interface()
    && !m::is_enum()
    && !m::any_vmethods(m::any<DexMethod>())
    // No dmethods which are annotated with anything in dont_optimize_annos
    && !m::any_dmethods(
      m::any_annos<DexMethod>(
        m::as_type<DexAnnotation>(m::in<DexType>(dont_optimize_annos))))
    && !m::any_sfields(m::any<DexField>())
    && !m::any_ifields(m::any<DexField>())
    // The only non-static dmethods should be default constructor
    && m::all_dmethods(
      (m::is_static<DexMethod>() && !m::is_constructor())
      || m::is_default_constructor())
    // Our class must not be referenced anywhere
    && !m::in<DexClass>(class_refs)
    // Make sure this class is not prohibited from being deleted. Granted,
    // we could still move methods and not delete the class, but let's
    // simplify things for now.
    && m::can_delete<DexClass>()
    && !m::any_annos<DexClass>(
        m::as_type<DexAnnotation>(m::in<DexType>(dont_optimize_annos)));

  visit_classes(scope, match, [&](DexClass* cls){
    TRACE(RELO, 5, "RELO %s is a candidate\n", SHOW(cls->get_type()));
    candidates.insert(cls);
  });

  return candidates;
}

/**
 * Builds a map of dex idx -> target DexClass* for relocation
 *
 * TODO: Maybe take PGO in here to choose the target better?
 *
 * @param candidates The set of all relocation candidates
 * @param cls_to_dex Map of DexClass* -> dex index
 * @return Unordered map of  dex idx -> target DexClass* for relocation
 */
std::unordered_map<int, DexClass*> build_dex_to_target_map(
  const candidates_t& candidates,
  const std::unordered_map<const DexClass*, int>& cls_to_dex) {
  std::unordered_map<int, DexClass*> map;
  for (DexClass* cls : candidates) {
    int dex = cls_to_dex.at(cls);
    map[dex] = cls;
  }
  for (const auto& it : map) {
    auto cls_name = SHOW(it.second->get_type());
    TRACE(RELO, 5, "RELO %s is target for dex %d\n", cls_name, it.first);
  }
  return map;
}

/**
 * Determines if 'method' is present in 'methods' based on
 * comparison of name/proto pairs (not raw DexMethod pointers)
 *
 * @param method The method we're testing
 * @param methods A vector of methods to check for collision
 * @return true if method name/proto matches a name/proto pair in methods.
 */
bool does_method_collide(
  const DexMethod* method,
  const std::vector<DexMethod*> methods) {
  for (auto other_method : methods) {
    if (method->get_name() == other_method->get_name() &&
      method->get_proto() == other_method->get_proto()) {
      return true;
    }
  }
  return false;
}

/**
 * Helper to add static (non-<clinit>) methods in 'target' to 'target_methods'
 *
 * @param target The class we'll add methods from
 * @param target_methods [out] A map of class to methods
 */
void add_target_methods(
  DexClass* target,
  std::unordered_map<DexClass*, std::vector<DexMethod*> >& target_methods) {
  if (target_methods.find(target) != target_methods.cend()) {
    // Already added target methods
    return;
  }
  for (auto m : target->get_dmethods()) {
    if (is_constructor(m)) continue;
    target_methods[target].push_back(m);
  }
  for (auto m : target->get_vmethods()) {
    if (is_constructor(m)) continue;
    target_methods[target].push_back(m);
  }
}

/**
 * This function selects the appropriate relocation target for 'meth'.
 * If there's only a single call site for 'meth', it should be relocated
 * there if it can. If it can criteria include:
 *
 * - Can't leave primary dex
 * - Can't collide
 *
 * If the meth does not have a single call site, or can't meet these
 * criteria, then we will use the default_target, provided that the method
 * does not collide with anything else on the default_target
 *
 * @param meth Method to find a relocation target for
 * @param dmethod_refs All of our demethod refs in the entire app
 * @param cls_to_pgo_order Map of class -> pgo order
 * @param cls_to_dex Map of class -> dex ordinal (e.g. 0..N)
 * @param target_methods [out] Methods on relocation targets, used to prevent
 *                       collisions
 * @return The relocation target, or nullptr if there is no valid target
 */
DexClass* select_relocation_target(
  const DexMethod* meth,
  DexClass* default_target,
  const refs_t<DexMethod>& dmethod_refs,
  const std::unordered_map<const DexClass*, int>& cls_to_pgo_order,
  const std::unordered_map<const DexClass*, int>& cls_to_dex,
  std::unordered_map<DexClass*, std::vector<DexMethod*> >& target_methods) {
/*
  const auto& refs = dmethod_refs.at(meth);
  if (refs.size() == 1) {
    const auto& ref = refs.at(0);
    DexClass* target = type_class(ref.first->get_class());
    always_assert(target);
    always_assert(!target->is_external());
    // Add target methods
    add_target_methods(target, target_methods);
    // Can't collide
    // N.B. [] rather than .at because target might not have any methods
    if (!does_method_collide(meth, target_methods[target])) {
      // Can't leave primary dex
      DexClass* donor = type_class(meth->get_class());
      if (!(cls_to_dex.at(donor) == 0 && cls_to_dex.at(target) != 0)) {
        s_single_ref_moved_count++;
        return target;
      }
    }
  }
*/
  // If not opportunity to relocate to a single call site, try the default
  // relocation target
    // N.B. [] rather than .at because default_target might not have any methods
  if (!does_method_collide(meth, target_methods[default_target])) {
    return default_target;
  }
  return nullptr;
}

/**
 * Builds all the mutations we'll make for relocation (method moves, method
 * deletes, class deletes).
 *
 * @param candidates Set of relocation candidates
 * @param dmethod_refs All references to dmethods in the entire program, used
 *        to find candidate call sites for relocation
 * @param cls_to_pgo_order Map of class -> cold start load rank
 * @param cls_to_dex Map of class -> dex index
 * @param meth_moves [out] Map of methods to move and what class to move to
 * @param meth_deletes [out] Set of methods to delete outright
 * @param cls_deletes [out] Set of classes to delete outright
 */
 void build_mutations(
  const candidates_t& candidates,
  const refs_t<DexMethod>& dmethod_refs,
  const std::unordered_map<const DexClass*, int>& cls_to_pgo_order,
  const std::unordered_map<const DexClass*, int>& cls_to_dex,
  const std::unordered_map<int, DexClass*>& dex_to_target,
  std::unordered_map<DexMethod*, DexClass*>& meth_moves,
  std::unordered_set<DexMethod*>& meth_deletes,
  std::unordered_set<DexClass*>& cls_deletes) {
  std::unordered_map<DexClass*, int> target_relocations;
  std::unordered_map<DexClass*, std::vector<DexMethod*> > target_methods;
  // Load the targets' existing methods into target_methods
  for (auto it : dex_to_target) {
    DexClass* target = it.second;
    add_target_methods(target, target_methods);
  }
  for (DexClass* cls : candidates) {
    // If we're a relocation target, completely skip us.
    DexClass* default_relocation_target = dex_to_target.at(cls_to_dex.at(cls));
    always_assert(default_relocation_target);
    if (default_relocation_target == cls) {
      TRACE(RELO, 5, "RELO %s is a relo target - not deleting\n", SHOW(cls));
      continue;
    }

    bool can_delete_class = true;
    for (DexMethod* meth : cls->get_dmethods()) {
      // Ignore non-statics
      if (!is_static(meth)) continue;
      // Must not have a static constructor (should have been filtered earlier)
      always_assert(!is_constructor(meth));

      if (dmethod_refs.find(meth) == dmethod_refs.end()) {
        // If the method is unreferenced, it may be deleted
        meth_deletes.insert(meth);
        TRACE(RELO, 5, "RELO %s is unreferenced; deleting\n", SHOW(meth));
      } else {
        // Count single call site opportunities
        if (dmethod_refs.at(meth).size() == 1) {
          s_single_ref_total_count++;
        }

        // If there's no relocation_target, we can't delete the class, and don't
        // move the method. We also need to make the method public as other
        // methods that were moved away may refer back to it.
        DexClass* relocation_target = select_relocation_target(
          meth,
          default_relocation_target,
          dmethod_refs,
          cls_to_pgo_order,
          cls_to_dex,
          target_methods);
        if (!relocation_target) {
          s_meth_could_not_move_count++;
          set_public(meth);
          can_delete_class = false;
          continue;
        }
        target_methods[relocation_target].push_back(meth);
        meth_moves[meth] = relocation_target;
        target_relocations[relocation_target]++;
      }
    }
    if (can_delete_class) {
      cls_deletes.insert(cls);
    }
  }

  // Calculate avg and max relocation load
  float total_relocations = 0.0f;
  for (const auto& kv : target_relocations) {
    total_relocations += kv.second;
    s_max_relocation_load = std::max(kv.second, s_max_relocation_load);
  }
  float target_relocations_count = target_relocations.size();
  s_avg_relocation_load = target_relocations_count ?
    total_relocations/target_relocations_count : 0.0f;
}

void delete_classes(
  Scope& scope,
  DexClassesVector& dexen,
  const std::unordered_set<DexClass*>& cls_deletes) {
  Scope tscope(scope);
  scope.clear();
  for (DexClass* cls : tscope) {
    if (cls_deletes.find(cls) == cls_deletes.end()) {
      scope.push_back(cls);
    } else {
      TRACE(RELO, 5, "RELO Deleting class %s\n", SHOW(cls));
    }
  }
  post_dexen_changes(scope, dexen);
}

void do_mutations(
  Scope& scope,
  DexClassesVector& dexen,
  std::unordered_map<DexMethod*, DexClass*>& meth_moves,
  std::unordered_set<DexMethod*>& meth_deletes,
  std::unordered_set<DexClass*>& cls_deletes) {

  // Do method deletes
  s_meth_delete_count = meth_deletes.size();
  for (auto& meth : meth_deletes) {
    type_class(meth->get_class())->get_dmethods().remove(meth);
  }

  // Do method moves. All the moves we're instructed to perform should
  // be valid here; all moves are obeyed.
  for (auto& it : meth_moves) {
    DexMethod* from_meth = it.first;
    DexClass* to_cls = it.second;
    // No reason to move the constructors, only move static methods
    if (is_static(from_meth)) {
      auto from_cls = type_class(from_meth->get_class());
      always_assert(from_cls != to_cls);
      TRACE(RELO, 5, "RELO Relocating %s to %s\n",
        SHOW(from_meth), SHOW(to_cls->get_type()));
      // Move the method to the target class
      from_cls->get_dmethods().remove(from_meth);
      from_meth->change_class(to_cls->get_type());
      insert_sorted(to_cls->get_dmethods(), from_meth, compare_dexmethods);
      // Make the method public and make the target class public. They must
      // be public because the method may have been visible to other other
      // call sites due to their own location (e.g. same package/class), but
      // the new placement may be restricted from those call sites without
      // these changes.
      set_public(from_meth);
      set_public(to_cls);
      s_meth_move_count++;
    }
  }

  // Do class deletes
  s_cls_delete_count = cls_deletes.size();
  delete_classes(scope, dexen, cls_deletes);
}

std::unordered_set<DexType*> get_dont_optimize_annos(
    folly::dynamic config, PgoFiles& pgo) {
  std::unordered_set<DexType*> dont;
  for (const auto& anno : pgo.get_no_optimizations_annos()) {
    dont.emplace(anno);
  }
  if (!config.isObject()) {
    return dont;
  }
  auto dont_item = config.find("dont_optimize_annos");
  if (dont_item == config.items().end()) {
    return dont;
  }
  auto const& dont_list = dont_item->second;
  for (auto const& dont_anno : dont_list) {
    if (dont_anno.isString()) {
      auto type = DexType::get_type(DexString::get_string(dont_anno.c_str()));
      if (type != nullptr) dont.emplace(type);
    }
  }
  return dont;
}

} // namespace

void StaticReloPass::run_pass(DexClassesVector& dexen, PgoFiles& pgo) {
  // Clear out counter
  s_cls_delete_count = 0;
  s_meth_delete_count = 0;
  s_meth_move_count = 0;
  s_meth_could_not_move_count = 0;
  s_avg_relocation_load = 0.0f;
  s_max_relocation_load = 0;
  s_single_ref_total_count = 0;
  s_single_ref_moved_count = 0;

  auto scope = build_class_scope(dexen);
  auto cls_to_dex = build_class_to_dex_map(dexen);
  auto cls_to_pgo_order = build_class_to_pgo_order_map(dexen, pgo);
  auto dont_optimize_annos = get_dont_optimize_annos(m_config, pgo);

  // Make one pass through all code to find dmethod refs and class refs,
  // needed later on for refining eligibility as well as performing the
  // actual rebinding
  refs_t<DexMethod> dmethod_refs;
  refs_t<DexClass> class_refs;
  build_refs(scope, dmethod_refs, class_refs);

  // Find candidates
  candidates_t candidates = build_candidates(
      scope, class_refs, dont_optimize_annos);

  // Find the relocation target for each dex
  auto dex_to_target = build_dex_to_target_map(
    candidates, cls_to_dex);

  // Build up all the mutations for relocation
  std::unordered_map<DexMethod*, DexClass*> meth_moves;
  std::unordered_set<DexMethod*> meth_deletes;
  std::unordered_set<DexClass*> cls_deletes;
  build_mutations(
    candidates,
    dmethod_refs,
    cls_to_pgo_order,
    cls_to_dex,
    dex_to_target,
    meth_moves,
    meth_deletes,
    cls_deletes);

  // Perform all relocation mutations
  do_mutations(scope, dexen, meth_moves, meth_deletes, cls_deletes);

  // Final report
  TRACE(
    RELO,
    1,
    "RELO :) Deleted %d methods\n"
    "RELO :) Moved %d methods\n"
    "RELO :) Deleted %d classes\n"
    "RELO :) Moved %d/%d methods to single call site targets\n"
    "RELO :| On average relocated %f methods onto all targets\n"
    "RELO :| Max %d methods relocated onto any one target\n"
    "RELO :( Could not move %d methods\n",
    s_meth_delete_count,
    s_meth_move_count,
    s_cls_delete_count,
    s_single_ref_moved_count,
    s_single_ref_total_count,
    s_avg_relocation_load,
    s_max_relocation_load,
    s_meth_could_not_move_count);
}
