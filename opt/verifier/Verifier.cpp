/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Verifier.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "Walkers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"

namespace {

using refs_t = std::unordered_map<const DexClass*,
                                  std::set<DexClass*, dexclasses_comparator>>;
using class_to_store_map_t = std::unordered_map<const DexClass*, DexStore*>;
using allowed_store_map_t = std::unordered_map<std::string, std::set<std::string>>;

/**
 * Helper function that scans all the opcodes in the application and produces a map of references
 * from the class containing the opcode to the class referenced by the opcode.
 *
 * @param scope all classes we're processing
 * @param dmethod_refs [out] all refs to dmethods in the application
 * @param class_refs [out] all refs to classes in the application
 *
 */
void build_refs(
    const Scope& scope,
    refs_t& class_refs) {
  // TODO: walk through annotations
  walk::opcodes(
    scope,
    [](const DexMethod*) { return true; },
    [&](const DexMethod* meth, IRInstruction* insn) {
      if (insn->has_type()) {
        const auto tref = type_class(insn->get_type());
        if (tref) class_refs[tref].emplace(type_class(meth->get_class()));
        return;
      }
      if (insn->has_field()) {
        const auto tref = type_class(insn->get_field()->get_class());
        if (tref) class_refs[tref].emplace(type_class(meth->get_class()));
        return;
      }
      if (insn->has_method()) {
        // log methods class type, for virtual methods, this may not actually exist and true
        // verification would require that the binding refers to a class that is valid.
        const auto mref = type_class(insn->get_method()->get_class());
        if (mref) class_refs[mref].emplace(type_class(meth->get_class()));

        // don't log return type or types of parameters for now, but this is how you might do it.
        //const auto proto = insn->get_method()->get_proto();
        // const auto rref = type_class(proto->get_rtype());
        // if (rref) class_refs[rref].emplace(type_class(meth->get_class()));
        // for (const auto arg : proto->get_args()->get_type_list()) {
        //   const auto aref = type_class(arg);
        //   if (aref) class_refs[aref].emplace(type_class(meth->get_class()));
        // }

        return;
      }
    });
}

DexStore& findStore(std::string& name, DexStoresVector& stores) {
  for (auto& store : stores) {
    if (name == store.get_name()) {
      return store;
    }
  }
  return stores[0];
}

const std::set<std::string> getAllowedStores(DexStoresVector& stores, DexStore& store, allowed_store_map_t store_map) {
  auto search = store_map.find(store.get_name());
  if (search != store_map.end()) {
    return search->second;
  }
  store_map[store.get_name()].emplace(store.get_name());
  store_map[store.get_name()].emplace(stores[0].get_name());
  for (auto parent : store.get_dependencies()) {
    store_map[store.get_name()].emplace(parent);
    for (auto grandparent : getAllowedStores(stores, findStore(parent, stores), store_map)) {
      store_map[store.get_name()].emplace(grandparent);
    }
  }
  return store_map[store.get_name()];
}

void verifyStore(DexStoresVector& stores, DexStore& store, class_to_store_map_t map, allowed_store_map_t store_map, FILE* fd) {
  refs_t class_refs;
  auto scope = build_class_scope(store.get_dexen());
  build_refs(scope, class_refs);
  for (auto& ref : class_refs) {
    const auto target = ref.first;
    for (const auto& source : ref.second) {
      std::string target_store_name;
      auto find = map.find(target);
      if (find != map.end()) {
        target_store_name = find->second->get_name();
      } else {
        target_store_name = "external";
      }
      std::set<std::string> allowed_stores = getAllowedStores(stores, store, store_map);
      if (allowed_stores.find(target_store_name) == allowed_stores.end()) {
        TRACE(
          VERIFY,
          5,
          "BAD REFERENCE from %s %s to %s %s\n",
          store.get_name().c_str(),
          source->get_deobfuscated_name().c_str(),
          target_store_name.c_str(),
          target->get_deobfuscated_name().c_str());
      }
      if (fd != nullptr) {
        fprintf(fd, "%s:%s->%s:%s\n",
          store.get_name().c_str(),
          source->get_deobfuscated_name().c_str(),
          target_store_name.c_str(),
          target->get_deobfuscated_name().c_str());
      }
    }
  }
}

} // namespace

void VerifierPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  m_class_dependencies_output = conf.metafile(m_class_dependencies_output);
  FILE* fd = nullptr;
  if (!m_class_dependencies_output.empty()) {
    fd = fopen(m_class_dependencies_output.c_str(), "w");
    if (fd == nullptr) {
      perror("Error opening class dependencies output file");
      return;
    }
  }

  allowed_store_map_t store_map;
  class_to_store_map_t map;
  for (auto& store : stores) {
    auto scope = build_class_scope(store.get_dexen());
    for (const auto& cls : scope) {
      map[cls] = &store;
    }
  }
  for (auto& store : stores) {
    verifyStore(stores, store, map, store_map, fd);
  }

  if (fd != nullptr) {
    fclose(fd);
  }
}

static VerifierPass s_pass;
