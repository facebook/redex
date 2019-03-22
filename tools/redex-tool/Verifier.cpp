/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Tool.h"
#include "Walkers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

using refs_t = std::unordered_map<const DexClass*,
                                  std::set<DexClass*, dexclasses_comparator>>;
using class_to_store_map_t = std::unordered_map<const DexClass*, DexStore*>;
using allowed_store_map_t =
    std::unordered_map<std::string, std::set<std::string>>;

namespace {

const DexStore& find_store(const std::string& name, const DexStoresVector& stores) {
  for (auto& store : stores) {
    if (name == store.get_name()) {
      return store;
    }
  }
  throw std::logic_error("Could not find store named "+name);
}

void build_allowed_stores_recurse(
  const DexStoresVector& stores,
  allowed_store_map_t& allowed_store_map,
  const DexStore& store,
  const DexStore& dep) {
  // Insert dep and recurse
  allowed_store_map[dep.get_name()].insert(store.get_name());
  // Insert each dep and recurse
  for (const auto& dep_dep_id : dep.get_dependencies()) {
    const auto& dep_dep = find_store(dep_dep_id, stores);
    build_allowed_stores_recurse(stores, allowed_store_map, store, dep_dep);
  }
}

/**
 * For each store, build a set of the stores it's allowed to be referred to by.
 *
 * Ex: STC depends on STB. STB depends on STA.
 *
 * STC -> { STC }
 * STB -> { STC, STB }
 * STA -> { STC, STB, STA }
 *
 */
void build_allowed_stores(
  const DexStoresVector& stores,
  allowed_store_map_t& allowed_store_map) {
  for (const auto& store : stores) {
    build_allowed_stores_recurse(stores, allowed_store_map, store, store);
  }
}

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

void verify(DexStoresVector& stores) {
  // Buld references map and class-to-store map
  refs_t class_refs;
  class_to_store_map_t cls_store_map;
  for (auto& store : stores) {
    auto scope = build_class_scope(store.get_dexen());
    for (const auto& cls : scope) {
      cls_store_map[cls] = &store;
    }
    build_refs(scope, class_refs);
  }
  int references = 0;
  for (const auto& refs : class_refs) {
    references += (int) refs.second.size();
  }

  // Build allowed stor (references) map
  allowed_store_map_t allowed_store_map;
  build_allowed_stores(stores, allowed_store_map);

  // Verify references
  for (const auto& store : stores) {
    // Validate that it's legal for each referer to see each reference.
    for (auto& ref : class_refs) {
      const auto reference = ref.first;
      for (const auto& referer : ref.second) {
        auto reference_store_it = cls_store_map.find(reference);
        auto referer_store_it = cls_store_map.find(referer);
        always_assert(referer_store_it != cls_store_map.end());
        if (reference_store_it != cls_store_map.end()) {
          std::string referer_store_name = referer_store_it->second->get_name();
          std::string reference_store_name = reference_store_it->second->get_name();
          std::set<std::string> allowed_stores = allowed_store_map[reference_store_name];
          if (allowed_stores.find(referer_store_name) == allowed_stores.end()) {
            fprintf(stderr,
              "ILLEGAL REFERENCE from %s %s to %s %s\n",
              store.get_name().c_str(),
              referer->get_name()->c_str(),
              reference_store_name.c_str(),
              reference->get_name()->c_str());
          }
        }
      }
    }
  }
}

} // namespace {

class Verifier : public Tool {
 public:
  Verifier() : Tool("verify", "verifies references between dexes") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options);
  }

  void run(const po::variables_map& options) override {
    auto stores = init(
      options["jars"].as<std::string>(),
      options["apkdir"].as<std::string>(),
      options["dexendir"].as<std::string>());
    verify(stores);
  }

 private:
};

static Verifier s_verifier;
