/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinTrivialLambdaDeduplicationPass.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>

#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "DexMemberRefs.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "KotlinLambdaAnalyzer.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "UniqueMethodTracker.h"
#include "Walkers.h"

namespace {

// Get the INSTANCE field from a lambda class.
DexField* get_instance_field(DexClass* cls, const DexString* instance_name) {
  const auto& sfields = cls->get_sfields();
  auto it =
      std::ranges::find_if(sfields, [cls, instance_name](DexField* field) {
        return field->get_name() == instance_name &&
               field->get_type() == cls->get_type();
      });
  return it != sfields.end() ? *it : nullptr;
}

// Build a map from lambda class types to their dex indices.
// Lower index means earlier dex file (e.g., classes.dex < classes2.dex).
// We pick the canonical lambda from the lowest-indexed dex because
// higher-indexed dexes can reference lower-indexed ones, but not vice versa.
//
// We don't use XDexRefs here because it builds a map for all classes, whereas
// we only need entries for lambda types in duplicate groups.
std::unordered_map<const DexType*, size_t> build_class_to_dex_idx_map(
    const DexStoresVector& stores,
    const UniqueMethodTracker& tracker,
    size_t min_duplicate_group_size) {
  // First, collect the lambda types we care about.
  std::unordered_set<const DexType*> lambda_types;
  for (const auto& [key, methods] : UnorderedIterable(tracker.groups())) {
    if (methods.size() >= min_duplicate_group_size) {
      for (const auto* method : UnorderedIterable(methods)) {
        lambda_types.insert(method->get_class());
      }
    }
  }

  // Then, find the dex index for each lambda type.
  std::unordered_map<const DexType*, size_t> result;
  size_t global_dex_idx = 0;
  for (const auto& store : stores) {
    for (const auto& dex : store.get_dexen()) {
      for (const auto* cls : dex) {
        if (lambda_types.contains(cls->get_type())) {
          result[cls->get_type()] = global_dex_idx;
        }
      }
      global_dex_idx++;
    }
  }
  return result;
}

} // namespace

void KotlinTrivialLambdaDeduplicationPass::run_pass(DexStoresVector& stores,
                                                    ConfigFiles& /* conf */,
                                                    PassManager& mgr) {
  Scope scope = build_class_scope(stores);

  // Cache string lookups to avoid repeated hash table lookups.
  const auto* const instance_name = DexString::make_string("INSTANCE");

  // Step 1: Collect all trivial lambdas and insert their invoke methods.
  UniqueMethodTracker tracker;

  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (!can_rename(cls) || !can_delete(cls)) {
      return;
    }

    auto analyzer = KotlinLambdaAnalyzer::analyze(cls);
    if (!analyzer.has_value() ||
        !analyzer->is_trivial(m_trivial_lambda_max_instructions)) {
      return;
    }

    if (get_instance_field(cls, instance_name) == nullptr) {
      // TODO(T251573078): Handle non-singleton (anonymous class) lambdas.
      return;
    }

    DexMethod* invoke = type::get_kotlin_lambda_invoke_method(cls);
    always_assert(invoke != nullptr);

    tracker.insert(invoke);
  });

  // Step 2: Check if any duplicate groups exist.
  size_t unique_signatures = tracker.size();
  {
    bool has_duplicates =
        std::any_of(UnorderedIterable(tracker.groups()).begin(),
                    UnorderedIterable(tracker.groups()).end(),
                    [this](const auto& entry) {
                      return entry.second.size() >= m_min_duplicate_group_size;
                    });
    if (!has_duplicates) {
      mgr.incr_metric("unique_signatures", unique_signatures);
      mgr.incr_metric("duplicate_groups", 0);
      mgr.incr_metric("trivial_lambdas_deduped", 0);
      TRACE(KOTLIN_INSTANCE, 1,
            "KotlinTrivialLambdaDeduplication: No duplicate trivial lambdas "
            "found.");
      return;
    }
  }

  // Step 3: Build a map from lambda types to their dex indices. We pick the
  // canonical lambda from the lowest-indexed dex (e.g., classes.dex <
  // classes2.dex) because higher-indexed dexes can reference lower-indexed
  // ones but not vice versa.
  const auto class_to_dex_idx =
      build_class_to_dex_idx_map(stores, tracker, m_min_duplicate_group_size);

  // Step 4: For each duplicate group, redirect all usages to the canonical
  // lambda's INSTANCE field. We rename the canonical's INSTANCE field to
  // prevent KotlinStatelessLambdaSingletonRemovalPass from processing it.
  size_t lambdas_deduplicated = 0;
  size_t duplicate_group_count = 0;

  // Map from original INSTANCE field to the canonical's renamed field.
  std::unordered_map<DexFieldRef*, DexFieldRef*> field_redirect_map;

  const auto* deduped_instance_name =
      DexString::make_string(kDedupedInstanceName);

  for (const auto& [key, methods] : UnorderedIterable(tracker.groups())) {
    if (methods.size() < m_min_duplicate_group_size) {
      continue;
    }
    duplicate_group_count++;

    // Find the lambda in the lowest-indexed dex to use as canonical.
    // Higher-indexed dexes can reference lower-indexed ones, so placing the
    // canonical in the lowest dex ensures all duplicates can reference it.
    DexClass* canonical = nullptr;
    size_t min_dex_idx = std::numeric_limits<size_t>::max();
    for (const auto* method : UnorderedIterable(methods)) {
      auto* lambda_cls = type_class(method->get_class());
      auto it = class_to_dex_idx.find(lambda_cls->get_type());
      size_t dex_idx = (it != class_to_dex_idx.end())
                           ? it->second
                           : std::numeric_limits<size_t>::max();
      if (dex_idx < min_dex_idx) {
        min_dex_idx = dex_idx;
        canonical = lambda_cls;
      }
    }
    always_assert(canonical != nullptr);

    DexField* canonical_instance = get_instance_field(canonical, instance_name);
    always_assert(canonical_instance != nullptr);

    // Rename the canonical's INSTANCE field to prevent
    // KotlinStatelessLambdaSingletonRemovalPass from processing it.
    DexFieldSpec new_spec{canonical_instance->get_class(),
                          deduped_instance_name,
                          canonical_instance->get_type()};
    canonical_instance->change(new_spec);

    // Map non-canonical lambdas in this group to use the canonical's field.
    // The canonical's field is renamed in place, so no redirect is needed for
    // code that already references it.
    for (const auto* method : UnorderedIterable(methods)) {
      auto* lambda_cls = type_class(method->get_class());
      if (lambda_cls == canonical) {
        continue;
      }
      DexField* instance_field = get_instance_field(lambda_cls, instance_name);
      always_assert(instance_field != nullptr);
      field_redirect_map[instance_field] = canonical_instance;
      lambdas_deduplicated++;
    }

    TRACE(KOTLIN_INSTANCE, 2,
          "KotlinTrivialLambdaDeduplication: Group with %zu lambdas, canonical "
          "= %s",
          methods.size(), SHOW(canonical));
  }

  // Step 5: Rewrite all usages of the original INSTANCE fields to use the
  // canonical's renamed field.
  const auto total_rewrites = walk::parallel::methods<size_t>(
      scope,
      [&field_redirect_map =
           std::as_const(field_redirect_map)](DexMethod* method) {
        size_t rewrites = 0;
        auto* code = method->get_code();
        if (code == nullptr) {
          return rewrites;
        }

        always_assert(code->cfg_built());
        auto& cfg = code->cfg();

        for (auto& mie : cfg::InstructionIterable(cfg)) {
          auto* insn = mie.insn;
          if (!opcode::is_an_sget(insn->opcode())) {
            continue;
          }

          auto* field = insn->get_field();
          auto it = field_redirect_map.find(field);
          if (it == field_redirect_map.end()) {
            continue;
          }

          // Redirect to the canonical's field.
          insn->set_field(it->second);
          rewrites++;
        }
        return rewrites;
      });

  // Report metrics.
  mgr.incr_metric("unique_signatures", unique_signatures);
  mgr.incr_metric("duplicate_groups", duplicate_group_count);
  mgr.incr_metric("trivial_lambdas_deduped", lambdas_deduplicated);
  mgr.incr_metric("instance_usages_rewritten", total_rewrites);

  TRACE(
      KOTLIN_INSTANCE, 1,
      "KotlinTrivialLambdaDeduplication: %zu unique signatures, %zu duplicate "
      "groups, %zu lambdas deduped",
      unique_signatures, duplicate_group_count, lambdas_deduplicated);
  TRACE(KOTLIN_INSTANCE, 1,
        "KotlinTrivialLambdaDeduplication: %zu instance usages rewritten",
        total_rewrites);
}

static KotlinTrivialLambdaDeduplicationPass s_pass;
