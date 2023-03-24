/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SpecializeRtype.h"

#include "ClassHierarchy.h"
#include "ConcurrentContainers.h"
#include "DexTypeEnvironment.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "Trace.h"
#include "WorkQueue.h"

namespace mog = method_override_graph;

namespace resolve_refs {

void RtypeStats::print(PassManager* mgr) const {
  if (num_rtype_specialized == 0) {
    return;
  }
  TRACE(RESO, 1, "[ref reso] rtype specialized %zu",
        num_rtype_specialized.load());
  TRACE(RESO, 1, "[ref reso] rtype specialized direct %zu",
        num_rtype_specialized_direct.load());
  TRACE(RESO, 1, "[ref reso] rtype specialized virtual 1 %zu",
        num_rtype_specialized_virtual_1.load());
  TRACE(RESO, 1, "[ref reso] rtype specialized virtual 1+ %zu",
        num_rtype_specialized_virtual_1p.load());
  TRACE(RESO, 1, "[ref reso] rtype specialized virtual 10+ %zu",
        num_rtype_specialized_virtual_10p.load());
  TRACE(RESO, 1, "[ref reso] rtype specialized virtual 100+ %zu",
        num_rtype_specialized_virtual_100p.load());
  TRACE(RESO, 1, "[ref reso] rtype specialized more override %zu",
        num_rtype_specialized_virtual_more_override.load());
  TRACE(RESO, 1, "[ref reso] rtype specialize virtual candidates %zu",
        num_virtual_candidates);
  mgr->incr_metric("num_rtype_specialized", num_rtype_specialized);
  mgr->incr_metric("num_rtype_specialized_direct",
                   num_rtype_specialized_direct);
  mgr->incr_metric("num_rtype_specialized_virtual_1",
                   num_rtype_specialized_virtual_1);
  mgr->incr_metric("num_rtype_specialized_virtual_1+",
                   num_rtype_specialized_virtual_1p);
  mgr->incr_metric("num_rtype_specialized_virtual_10+",
                   num_rtype_specialized_virtual_10p);
  mgr->incr_metric("num_rtype_specialized_virtual_100+",
                   num_rtype_specialized_virtual_100p);
  mgr->incr_metric("num_rtype_specialized_virtual_more_override+",
                   num_rtype_specialized_virtual_more_override);
  mgr->incr_metric("num_virtual_candidates", num_virtual_candidates);
}

namespace {

/*
 * Mirroring the similar checks we have in RemoveUnusedArgsPass.
 */
bool is_safe_to_specialize(const DexMethod* meth) {
  return can_delete(meth) && can_rename(meth) && !is_native(meth);
}

bool can_update_rtype_for(DexMethod* meth, const DexProto* new_proto) {
  if (!is_safe_to_specialize(meth)) {
    return false;
  }
  if (DexMethod::get_method(meth->get_class(), meth->get_name(), new_proto)) {
    // Bail on collision.
    TRACE(RESO, 4, "specialize bail on proto collision w/ %s -> %s", SHOW(meth),
          SHOW(new_proto));
    return false;
  }
  auto* resolved = resolve_method(type_class(meth->get_class()),
                                  meth->get_name(), new_proto);
  if (resolved) {
    // Bail on virtual scope collision.
    TRACE(RESO, 4, "specialize bail on virtual scope collision w/ %s",
          SHOW(resolved));
    return false;
  }
  return true;
}

bool can_update_rtype_for(DexMethod* meth, const DexType* new_rtype) {
  DexProto* updated_proto =
      DexProto::make_proto(new_rtype, meth->get_proto()->get_args());
  return can_update_rtype_for(meth, updated_proto);
}

bool can_update_rtype_for_list(const std::vector<const DexMethod*>& meths,
                               const DexProto* new_proto) {
  for (auto* m : meths) {
    auto* meth = const_cast<DexMethod*>(m);
    if (!can_update_rtype_for(meth, new_proto)) {
      return false;
    }
  }
  return true;
}

bool share_common_rtype_candidate(
    const MethodToInferredReturnType& rtype_candidates,
    const std::vector<const DexMethod*>& meths,
    const DexType* better_rtype) {
  for (auto* m : meths) {
    if (!m->get_code()) {
      continue;
    }
    auto* meth = const_cast<DexMethod*>(m);
    if (rtype_candidates.count(meth) == 0) {
      return false;
    }
    const auto* candidate_rtype = rtype_candidates.at(meth);
    if (candidate_rtype != better_rtype) {
      TRACE(RESO, 4, "overridden mismatch better rtype %s -> %s vs %s",
            SHOW(candidate_rtype), SHOW(candidate_rtype), SHOW(better_rtype));
      return false;
    }
  }

  return true;
}

void update_rtype_for(DexMethod* meth,
                      const DexType* new_rtype,
                      RtypeStats& stats,
                      bool rename_on_collision = false) {
  DexProto* updated_proto =
      DexProto::make_proto(new_rtype, meth->get_proto()->get_args());
  if (!can_update_rtype_for(meth, updated_proto)) {
    return;
  }

  DexMethodSpec spec(nullptr, nullptr, updated_proto);
  meth->change(spec, rename_on_collision);
  TRACE(RESO, 4, "rtype specialized -> %s", SHOW(meth));
  stats.num_rtype_specialized++;
}

void update_rtype_unsafe_for(DexMethod* meth,
                             const DexType* new_rtype,
                             RtypeStats& stats) {
  DexProto* updated_proto =
      DexProto::make_proto(new_rtype, meth->get_proto()->get_args());
  DexMethodSpec spec(nullptr, nullptr, updated_proto);
  meth->change(spec, false);
  TRACE(RESO, 4, "rtype specialized -> %s", SHOW(meth));
  stats.num_rtype_specialized++;
}

bool update_rtype_for_list(const std::vector<const DexMethod*>& meths,
                           const DexType* new_rtype,
                           RtypeStats& stats) {
  if (meths.empty()) {
    return true;
  }
  DexProto* updated_proto =
      DexProto::make_proto(new_rtype, meths.front()->get_proto()->get_args());

  for (auto* m : meths) {
    auto* meth = const_cast<DexMethod*>(m);
    if (!can_update_rtype_for(meth, updated_proto)) {
      return false;
    }
  }

  for (auto* m : meths) {
    auto* meth = const_cast<DexMethod*>(m);
    update_rtype_unsafe_for(meth, new_rtype, stats);
  }

  return true;
}

} // namespace

void RtypeCandidates::collect_inferred_rtype(
    const DexMethod* meth,
    const DexTypeDomain& inferred_rtype,
    DexTypeDomain& curr_rtype) {
  // Method itself is not qualified for proto rtype update
  if (!is_safe_to_specialize(meth)) {
    return;
  }

  TRACE(RESO, 4, "collect rtype for %s inferred %s", SHOW(meth),
        SHOW(inferred_rtype));
  curr_rtype.join_with(inferred_rtype);
}

void RtypeCandidates::collect_specializable_rtype(
    DexMethod* meth, const DexTypeDomain& rtype_domain) {
  if (rtype_domain.is_bottom() || rtype_domain.is_top()) {
    return;
  }
  DexType* rtype = meth->get_proto()->get_rtype();
  const auto better_rtype = rtype_domain.get_dex_type();
  if (!better_rtype || *better_rtype == type::java_lang_Object()) {
    return;
  }
  redex_assert(type::is_object(rtype));
  redex_assert(better_rtype);
  if (*better_rtype == rtype || type::is_array(rtype)) {
    return;
  }
  // `better_rtype` is a subtype of the exsting `rtype`.
  if (type::check_cast(*better_rtype, rtype) &&
      can_update_rtype_for(meth, *better_rtype)) {
    m_rtype_candidates.emplace(meth, *better_rtype);
  }
}

bool RtypeSpecialization::shares_identical_rtype_candidate(
    DexMethod* meth, const DexType* better_rtype) const {
  if (!meth->get_code()) {
    return true;
  }

  if (m_candidates.count(meth) == 0) {
    return false;
  }
  const auto* candidate_rtype = m_candidates.at(meth);
  if (candidate_rtype != better_rtype) {
    return false;
  }

  return true;
}

void RtypeSpecialization::specialize_non_true_virtuals(
    const method_override_graph::Graph& override_graph,
    DexMethod* meth,
    const DexType* better_rtype,
    ConcurrentMap<DexMethod*, const DexType*>& virtual_roots,
    RtypeStats& stats) const {
  const auto& overridings =
      method_override_graph::get_overriding_methods(override_graph, meth, true);
  always_assert(overridings.empty());
  virtual_roots.emplace(meth, better_rtype);
  stats.num_rtype_specialized_direct++;
}

void RtypeSpecialization::specialize_true_virtuals(
    const method_override_graph::Graph& override_graph,
    DexMethod* meth,
    const DexType* better_rtype,
    ConcurrentMap<DexMethod*, const DexType*>& virtual_roots,
    RtypeStats& stats) const {
  const auto& overridings =
      method_override_graph::get_overriding_methods(override_graph, meth, true);
  if (!overridings.empty()) {
    // If a candidate is overriden by another concrete method, we give up.
    // We try to avoid more complex cases here. We can potentially expand on
    // this.
    stats.num_rtype_specialized_virtual_more_override++;
    return;
  }
  const auto& overriddens =
      method_override_graph::get_overridden_methods(override_graph, meth, true);
  if (overriddens.size() > 1) {
    // Too many overriddens. Give up
    return;
  }
  if (overriddens.size() == 1) {
    // For now, we only focus on methods with one abstract overriden here.
    DexMethod* overridden = const_cast<DexMethod*>(*overriddens.begin());
    if (!is_safe_to_specialize(overridden) || overridden->is_external()) {
      // Overridden has code or is external. Give up
      TRACE(RESO, 4, "specialize bail on more complex overridden %s",
            SHOW(overridden));
      return;
    }

    if (!shares_identical_rtype_candidate(overridden, better_rtype)) {
      // The overriden has to share the identical `better_rtype` to be
      // specialized
      return;
    }

    const auto& global_overridings =
        method_override_graph::get_overriding_methods(override_graph,
                                                      overridden, true);
    if (global_overridings.size() > 100) {
      stats.num_rtype_specialized_virtual_100p++;
      return;
    } else if (global_overridings.size() > 10) {
      stats.num_rtype_specialized_virtual_10p++;
      return;
    }
    if (global_overridings.size() > 1) {
      stats.num_rtype_specialized_virtual_1p++;
      if (can_update_rtype_for(overridden, better_rtype) &&
          share_common_rtype_candidate(m_candidates, global_overridings,
                                       better_rtype)) {
        TRACE(RESO, 4, "global overrides %ld -> %s ", global_overridings.size(),
              SHOW(better_rtype));
        DexProto* updated_proto =
            DexProto::make_proto(better_rtype, meth->get_proto()->get_args());
        if (can_update_rtype_for_list(global_overridings, updated_proto)) {
          virtual_roots.emplace(overridden, better_rtype);
        }
      }
      return;
    }
    TRACE(RESO, 4, "specialize virtual 1 overridden %s w/ rtype %s", SHOW(meth),
          SHOW(better_rtype));
    if (can_update_rtype_for(overridden, better_rtype) &&
        can_update_rtype_for(meth, better_rtype)) {
      stats.num_rtype_specialized_virtual_1++;
      virtual_roots.emplace(overridden, better_rtype);
      TRACE(RESO, 4, "root virtual 1 overridden %s w/ rtype %s",
            SHOW(overridden), SHOW(better_rtype));
    }
  } else {
    not_reached_log("true virtual w/ 0 overridden & 0 overridding %s",
                    SHOW(meth));
  }
}

void RtypeSpecialization::specialize_rtypes(const Scope& scope) {
  Timer t("specialize_rtype");
  const auto& override_graph = method_override_graph::build_graph(scope);
  ConcurrentMap<DexMethod*, const DexType*> virtual_roots;

  // Preprocess the candidates to cut down the size of candidates.
  // The main logic is filtering out complex virtual scopes that we choose not
  // to touch.
  auto process_candidates =
      [&](const std::pair<DexMethod*, const DexType*>& pair) {
        auto* meth = pair.first;
        const auto* better_rtype = const_cast<DexType*>(pair.second);

        if (!meth->is_virtual()) {
          // Simple direct methods. Fall through to 2nd step.
        } else if (!method_override_graph::is_true_virtual(*override_graph,
                                                           meth)) {
          // Non true virtual methods.
          TRACE(RESO, 4, "specialize non true virtual %s w/ rtype %s",
                SHOW(meth), SHOW(better_rtype));
          specialize_non_true_virtuals(*override_graph, meth, better_rtype,
                                       virtual_roots, m_stats);
        } else {
          specialize_true_virtuals(*override_graph, meth, better_rtype,
                                   virtual_roots, m_stats);
        }
      };

  workqueue_run<std::pair<DexMethod*, const DexType*>>(process_candidates,
                                                       m_candidates);

  // Update direct targets.
  for (auto& pair : m_candidates) {
    auto* meth = pair.first;
    if (meth->is_virtual()) {
      continue;
    }
    const auto* better_rtype = pair.second;
    update_rtype_for(meth, better_rtype, m_stats);
    TRACE(RESO, 4, "specialize direct %s w/ rtype %s", SHOW(meth),
          SHOW(better_rtype));
    m_stats.num_rtype_specialized_direct++;
  }

  // Sort and update virtual targets.
  m_stats.num_virtual_candidates = virtual_roots.size();
  std::vector<DexMethod*> virtuals_lst;
  for (auto& pair : virtual_roots) {
    virtuals_lst.push_back(pair.first);
  }
  std::sort(virtuals_lst.begin(), virtuals_lst.end(), compare_dexmethods);

  ClassHierarchy ch = build_type_hierarchy(scope);
  for (auto* root : virtuals_lst) {
    const auto* better_rtype = virtual_roots.at(root);
    const auto& overrides = method_override_graph::get_overriding_methods(
        *override_graph, root, true);
    auto* new_proto =
        DexProto::make_proto(better_rtype, root->get_proto()->get_args());
    auto* cls = type_class(root->get_class());
    if (find_collision(ch, root->get_name(), new_proto, cls,
                       /* is_virtual */ true)) {
      TRACE(RESO, 4, "Bail on virtual collision %s w/ rtype %s", SHOW(root),
            SHOW(better_rtype));
      continue;
    }
    if (update_rtype_for_list(overrides, better_rtype, m_stats)) {
      update_rtype_for(root, better_rtype, m_stats);
    }
  }
}

} // namespace resolve_refs
