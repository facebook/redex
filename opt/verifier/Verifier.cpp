/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Verifier.h"

#include <algorithm>
#include <cinttypes>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

const std::string CLASS_DEPENDENCY_FILENAME = "redex-class-dependencies.txt";

using refs_t = std::unordered_map<
    const DexStore*,
    ConcurrentMap<const DexClass*, std::unordered_set<DexClass*>>>;
using class_to_store_map_t = std::unordered_map<const DexClass*, DexStore*>;
using allowed_store_map_t =
    std::unordered_map<std::string, std::unordered_set<std::string>>;

/**
 * Helper function that scans all the opcodes in the application and produces a
 * map of references from the class containing the opcode to the class
 * referenced by the opcode.
 *
 * @param scope all classes we're processing
 * @param dmethod_refs [out] all refs to dmethods in the application
 * @param class_refs [out] all refs to classes in the application
 *
 */
void build_refs(const Scope& scope,
                const class_to_store_map_t& map,
                refs_t& class_refs) {
  // TODO: walk through annotations
  walk::parallel::classes(scope, [&](DexClass* cls) {
    auto& store_class_refs = class_refs.at(map.at(cls));
    auto add_ref = [&](DexClass* target) {
      if (target) {
        store_class_refs.update(
            target, [cls](auto, auto& set, auto) { set.insert(cls); });
      }
    };

    walk::opcodes(
        std::vector{cls}, [&](const DexMethod*, const IRInstruction* insn) {
          if (insn->has_type()) {
            const auto tref = type_class(insn->get_type());
            add_ref(tref);
            return;
          }
          if (insn->has_field()) {
            const auto tref = type_class(insn->get_field()->get_class());
            add_ref(tref);
            return;
          }
          if (insn->has_method()) {
            // log methods class type, for virtual methods, this may not
            // actually exist and true verification would require that the
            // binding refers to a class that is valid.
            const auto mref = type_class(insn->get_method()->get_class());
            add_ref(mref);

            // don't log return type or types of parameters for now, but this is
            // how you might do it.
            // const auto proto = insn->get_method()->get_proto();
            // const auto rref = type_class(proto->get_rtype());
            // if (rref) add_ref(rref);
            // for (const auto arg : proto->get_args()->get_type_list()) {
            //   const auto aref = type_class(arg);
            //   add_ref(aref);
            // }

            return;
          }
        });
  });
}

const DexStore& findStore(std::string& name, const DexStoresVector& stores) {
  for (auto& store : stores) {
    if (name == store.get_name()) {
      return store;
    }
  }
  return stores[0];
}

const std::unordered_set<std::string>& getAllowedStores(
    const DexStoresVector& stores,
    const DexStore& store,
    allowed_store_map_t& store_map) {
  const auto& name = store.get_name();
  auto search = store_map.find(name);
  if (search != store_map.end()) {
    return search->second;
  }
  std::unordered_set<std::string> map;
  map.emplace(name);
  map.emplace(stores[0].get_name());
  for (auto parent : store.get_dependencies()) {
    map.emplace(parent);
    for (const auto& grandparent :
         getAllowedStores(stores, findStore(parent, stores), store_map)) {
      map.emplace(grandparent);
    }
  }
  auto [it, emplaced] = store_map.emplace(name, std::move(map));
  always_assert(emplaced);
  return it->second;
}

uint64_t verifyStore(const DexStoresVector& stores,
                     const DexStore& store,
                     const class_to_store_map_t& map,
                     const refs_t& class_refs,
                     allowed_store_map_t& store_map,
                     FILE* fd) {
  const auto& allowed_stores = getAllowedStores(stores, store, store_map);
  uint64_t dependencies{0};
  for (auto& [target, sources] : class_refs.at(&store)) {
    always_assert(!sources.empty());
    auto find = map.find(target);
    static const std::string external_store_name = "external";
    const std::string& target_store_name =
        find != map.end() ? find->second->get_name() : external_store_name;

    if (!allowed_stores.count(target_store_name)) {
      for (const auto& source : sources) {
        TRACE(VERIFY, 5, "BAD REFERENCE from %s %s to %s %s",
              store.get_name().c_str(), show_deobfuscated(source).c_str(),
              target_store_name.c_str(), show_deobfuscated(target).c_str());
      }
    }
    if (fd != nullptr) {
      std::string target_deobfuscated = show_deobfuscated(target);
      for (const auto& source : sources) {
        fprintf(fd, "%s:%s->%s:%s\n", store.get_name().c_str(),
                show_deobfuscated(source).c_str(), target_store_name.c_str(),
                target_deobfuscated.c_str());
      }
    }
    dependencies += sources.size();
  }
  return dependencies;
}

} // namespace

void VerifierPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  auto class_dep_out = conf.metafile(CLASS_DEPENDENCY_FILENAME);
  FILE* fd = fopen(class_dep_out.c_str(), "w");

  if (fd == nullptr) {
    perror("Error opening class dependencies output file");
    return;
  }

  allowed_store_map_t store_map;
  class_to_store_map_t map;
  refs_t class_refs;
  for (auto& store : stores) {
    auto scope = build_class_scope(store.get_dexen());
    for (const auto& cls : scope) {
      map[cls] = &store;
    }
    class_refs[&store];
  }

  auto scope = build_class_scope(stores);
  build_refs(scope, map, class_refs);

  uint64_t dependencies{0};
  for (auto& store : stores) {
    dependencies += verifyStore(stores, store, map, class_refs, store_map, fd);
  }

  if (fd != nullptr) {
    fclose(fd);
  }

  TRACE(VERIFY, 1, "%" PRIu64 " dependencies found", dependencies);
  mgr.incr_metric("dependencies", dependencies);
}

static VerifierPass s_pass;
