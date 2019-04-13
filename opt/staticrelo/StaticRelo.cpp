/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StaticRelo.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Match.h"
#include "ConfigFiles.h"
#include "Walkers.h"

namespace {

#define METRIC_NUM_CANDIDATE_CLASSES "num_candidate_classes"
#define METRIC_NUM_DELETED_CLASSES "**num_deleted_classes**"
#define METRIC_NUM_MOVED_METHODS "num_moved_methods"
#define METRIC_NUM_DELETED_METHODS "num_deleted_methods"


// Counters for this optimization
static int s_meth_could_not_move_count;
static float s_avg_relocation_load;
static size_t s_max_relocation_load;
static int s_single_ref_total_count;
static int s_single_ref_moved_count;

/** map of dmethod or class (T) -> method/opcode referencing dmethod or class */
template <typename T>
using refs_t = std::unordered_map<
  const T*, std::vector<std::pair<const DexMethod*, IRInstruction*> > >;

/** all relocation candidate classes */
using candidates_t = std::set<DexClass*, dexclasses_comparator>;

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
 * @param p A match_t<IRInstruction, ...> built up by m::* routines
 * @param v Visitor function
 */
template<typename P, typename V = void(DexMethod*, IRInstruction*)>
void visit_opcodes(
  const Scope& scope, const m::match_t<IRInstruction, P>& p, const V& v) {
  walk::opcodes(
    scope,
    [](const DexMethod*) { return true; },
    [&](const DexMethod* m, IRInstruction* insn) {
      if (p.matches(insn)) {
        v(m, insn);
      }
    });
}

/**
 * Helper to build a map of DexClass* -> dex index
 *
 * @param dexen All classes in the dexen
 * @return Unordered map of DexClass* -> dex index
 */
std::unordered_map<const DexClass*, size_t> build_class_to_dex_map(
  const DexClassesVector& dexen) {
  std::unordered_map<const DexClass*, size_t> map;
  for (size_t i = 0, N = dexen.size() ; i < N ; ++i) {
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
 * @param conf Our PGO input files
 * @return Unordered map of DexClass* -> cold start class load rank.
 *         Lower rank is earlier in cold start class load.
 */
std::unordered_map<const DexClass*, size_t> build_class_to_pgo_order_map(
    const DexClassesVector& dexen, ConfigFiles& conf) {
  auto interdex_list = conf.get_coldstart_classes();
  std::unordered_map<std::string, DexClass*> class_string_map;
  std::unordered_map<const DexClass*, size_t> coldstart_classes;
  for (auto const& dex : dexen) {
    for (auto const& cls : dex) {
      class_string_map[cls->get_type()->get_name()->str()] = cls;
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
 * @param referenced_types [out] all refs not coming from instructions (e.g. catch blocks)
 *
 */
void build_refs(
    const Scope& scope,
    refs_t<DexMethodRef>& dmethod_refs,
    refs_t<DexClass>& class_refs,
    std::unordered_set<DexClass*>& referenced_types) {
  // Looking for direct/static invokes or class refs
  auto match =
    m::invoke_static()
    || m::invoke_direct()
    || m::has_type();
  visit_opcodes(scope, match, [&](const DexMethod* meth, IRInstruction* insn){
    if (insn->has_type()) {
      const auto tref = type_class(insn->get_type());
      if (tref) class_refs[tref].push_back(std::make_pair(meth, insn));
    } else {
      const auto mref = insn->get_method();
      dmethod_refs[mref].push_back(std::make_pair(meth, insn));
    }
  });
  // collect all exceptions and add to the set of references for the app
  walk::code(scope,
      [&](DexMethod* method, IRCode& code) {
        std::vector<DexType*> exceptions;
        code.gather_catch_types(exceptions);
        for (const auto& exception : exceptions) {
          auto cls = type_class(exception);
          if (cls == nullptr || cls->is_external()) continue;
          referenced_types.insert(cls);
        }
      });
}

/**
 * Builds a set of classes which are candidates for having their static
 * methods relocated.
 *
 * @param scope all classes we're processing
 * @param class_refs class refs for the application
 * @param referenced_types all refs not coming from instructions (e.g. catch blocks)
 * @param dont_optimize_annos set of anno types which, if present on a method,
 *                            should prevent use from relocating that method.
 * @return an unordered set of DexClass which are static relocation candidates
 */
candidates_t build_candidates(
  const Scope& scope,
  const refs_t<DexClass>& class_refs,
  const std::unordered_set<DexClass*>& referenced_types,
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
    && m::has_class_data()
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
    && !m::in<DexClass>(referenced_types)
    // Make sure this class is not prohibited from being deleted. Granted,
    // we could still move methods and not delete the class, but let's
    // simplify things for now.
    && !m::has_keep<DexClass>()
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
std::unordered_map<size_t, DexClass*> build_dex_to_target_map(
  const candidates_t& candidates,
  const std::unordered_map<const DexClass*, size_t>& cls_to_dex) {
  std::unordered_map<size_t, DexClass*> map;
  for (DexClass* cls : candidates) {
    size_t dex = cls_to_dex.at(cls);
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
  const refs_t<DexMethodRef>& dmethod_refs,
  const std::unordered_map<const DexClass*, size_t>& cls_to_pgo_order,
  const std::unordered_map<const DexClass*, size_t>& cls_to_dex,
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
 * Check if references inside a candidate method can be moved.
 */
bool can_make_references_public(const DexMethod* from_meth) {
  auto const& code = from_meth->get_code();
  if (!code) return false;
  for (auto const& mie : InstructionIterable(code)) {
    auto inst = mie.insn;
    if (inst->has_type()) {
      auto tref = inst->get_type();
      auto tclass = type_class(tref);
      if (!tclass) return false;
      if (tclass->is_external() && !is_public(tclass)) return false;
    } else if (inst->has_field()) {
      auto fref = resolve_field(inst->get_field());
      if (!fref) return false;
      auto fclass = type_class(fref->get_class());
      if (!fclass) return false;
      if (fref->is_external() && (!is_public(fref) || !is_public(fclass))) {
        return false;
      }
    } else if (inst->has_method()) {
      auto mref = resolve_method(inst->get_method(), opcode_to_search(inst));
      if (!mref) return false;
      auto mclass = type_class(mref->get_class());
      if (!mclass) return false;
      if (mref->is_external() && (!is_public(mref) || !is_public(mclass))) {
        return false;
      }
    }
  }
  return true;
}

/**
 * A moved method may refer to package private members.  Make things public as
 * needed.
 */
void make_references_public(const DexMethod* from_meth) {
  auto const& code = from_meth->get_code();
  if (!code) return;
  for (auto const& mie : InstructionIterable(code)) {
    auto inst = mie.insn;
    if (inst->has_type()) {
      auto tref = inst->get_type();
      auto tclass = type_class(tref);
      always_assert(tclass);
      if (!tclass->is_external()) set_public(tclass);
    } else if (inst->has_field()) {
      auto fref = resolve_field(inst->get_field());
      auto fclass = type_class(fref->get_class());
      always_assert(fclass);
      if (fref->is_concrete()) {
        set_public(fclass);
        set_public(fref);
      }
    } else if (inst->has_method()) {
      auto mref = resolve_method(inst->get_method(), opcode_to_search(inst));
      auto mclass = type_class(mref->get_class());
      always_assert(mclass);
      if (mref->is_concrete()) {
        set_public(mclass);
        set_public(mref);
      }
    }
  }
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
  const refs_t<DexMethodRef>& dmethod_refs,
  const std::unordered_map<const DexClass*, size_t>& cls_to_pgo_order,
  const std::unordered_map<const DexClass*, size_t>& cls_to_dex,
  const std::unordered_map<size_t, DexClass*>& dex_to_target,
  std::unordered_map<DexMethod*, DexClass*>& meth_moves,
  std::unordered_set<DexMethod*>& meth_deletes,
  std::unordered_set<DexClass*>& cls_deletes) {
  std::unordered_map<DexClass*, size_t> target_relocations;
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
        // We need to make any references in the candidate public; if we can't,
        // then we can't move the class.
        if (!can_make_references_public(meth)) {
          s_meth_could_not_move_count++;
          can_delete_class = false;
          continue;
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
        always_assert_log(relocation_target->has_class_data(),
            "Relocation target %s has no class data\n",
            SHOW(relocation_target->get_type()));
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

void record_move_data(DexMethod* from_meth,
                      DexClass* from_cls,
                      DexClass* to_cls,
                      ConfigFiles& conf) {
  MethodTuple from_tuple = std::make_tuple(
      from_cls->get_type()->get_name(),
      from_meth->get_name(),
      from_cls->get_source_file());
  conf.add_moved_methods(from_tuple, to_cls);
}

void do_mutations(PassManager& mgr,
                  Scope& scope,
                  DexClassesVector& dexen,
                  std::unordered_map<DexMethod*, DexClass*>& meth_moves,
                  const std::unordered_set<DexMethod*>& meth_deletes,
                  const std::unordered_set<DexClass*>& cls_deletes,
                  ConfigFiles& conf) {

  for (auto& meth : meth_deletes) {
    type_class(meth->get_class())->remove_method(meth);
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
      if (from_cls->get_type()->get_name()->c_str() == nullptr ||
          from_meth->get_name()->c_str() == nullptr) {
        TRACE(RELO, 5, "skipping class move\n");
        continue;
      }
      record_move_data(from_meth, from_cls, to_cls, conf);
      // Move the method to the target class
      from_cls->remove_method(from_meth);
      DexMethodSpec spec;
      spec.cls = to_cls->get_type();
      from_meth->change(spec,
                        true /* rename on collision */,
                        true /* update deobfuscated name */);
      to_cls->add_method(from_meth);
      // Make the method public and make the target class public. They must
      // be public because the method may have been visible to other other
      // call sites due to their own location (e.g. same package/class), but
      // the new placement may be restricted from those call sites without
      // these changes.
      set_public(from_meth);
      make_references_public(from_meth);
      set_public(to_cls);
      mgr.incr_metric(METRIC_NUM_MOVED_METHODS, 1);
    }
  }

  // Do class deletes
  delete_classes(scope, dexen, cls_deletes);
}

std::unordered_set<DexType*> get_dont_optimize_annos(
    const std::vector<std::string>& dont_list, ConfigFiles& conf) {
  std::unordered_set<DexType*> dont;
  for (const auto& anno : conf.get_no_optimizations_annos()) {
    dont.emplace(anno);
  }
  for (auto const& dont_anno : dont_list) {
    auto type = DexType::get_type(DexString::get_string(dont_anno.c_str()));
    if (type != nullptr) dont.emplace(type);
  }
  return dont;
}

} // namespace

void StaticReloPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(RELO, 1, "StaticReloPass not run because no ProGuard configuration was provided.");
    return;
  }
  // Clear out counter
  s_meth_could_not_move_count = 0;
  s_avg_relocation_load = 0.0f; // FIXME: does not work with DexStores
  s_max_relocation_load = 0; // FIXME: does not work with DexStores
  s_single_ref_total_count = 0;
  s_single_ref_moved_count = 0;

  //relocate statics on a per-dex store basis
  for (auto& store : stores) {
    DexClassesVector& dexen = store.get_dexen();
    auto scope = build_class_scope(dexen);
    auto cls_to_dex = build_class_to_dex_map(dexen);
    auto cls_to_pgo_order = build_class_to_pgo_order_map(dexen, conf);
    auto dont_optimize_annos =
        get_dont_optimize_annos(m_dont_optimize_annos, conf);

    // Make one pass through all code to find dmethod refs and class refs,
    // needed later on for refining eligibility as well as performing the
    // actual rebinding
    refs_t<DexMethodRef> dmethod_refs;
    refs_t<DexClass> class_refs;
    std::unordered_set<DexClass*> referenced_types;

    build_refs(scope, dmethod_refs, class_refs, referenced_types);

    // Find candidates
    candidates_t candidates = build_candidates(
        scope, class_refs, referenced_types, dont_optimize_annos);
    mgr.incr_metric(METRIC_NUM_CANDIDATE_CLASSES, candidates.size());

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
    mgr.incr_metric(METRIC_NUM_DELETED_CLASSES, cls_deletes.size());
    mgr.incr_metric(METRIC_NUM_DELETED_METHODS, meth_deletes.size());

    // Perform all relocation mutations
    do_mutations(mgr, scope, dexen, meth_moves, meth_deletes, cls_deletes,
                 conf);
  }

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
    "RELO :( Could not move %d methods\n"
    "aliasing criteria\n",
    mgr.get_metric(METRIC_NUM_DELETED_METHODS),
    mgr.get_metric(METRIC_NUM_MOVED_METHODS),
    mgr.get_metric(METRIC_NUM_DELETED_CLASSES),
    s_single_ref_moved_count,
    s_single_ref_total_count,
    s_avg_relocation_load,
    s_max_relocation_load,
    s_meth_could_not_move_count);
}

static StaticReloPass s_pass;
