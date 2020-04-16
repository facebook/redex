/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InitCollisionFinder.h"

#include "DexUtil.h"

/**
 * Some optimizations want to change the prototypes of many methods. Sometimes,
 * changing those method prototypes will collide with another method. For most
 * method collisions we rename the new method to avoid the collision. But we
 * cannot rename <init> methods.
 *
 * This utility works around the init collision problem by finding the types
 * that cause the init collision. This allows an optimization to exclude these
 * types before it makes any changes.
 */

namespace init_collision_finder {

/**
 * Given a method, return the new DexMethodSpec that the optimization wants to
 * change this method to (returning boost::none if it does not want to make a
 * change). In the process, this method should fill the vector argument with any
 * DexTypes that were replaced in the method's prototype.
 *
 * This function is supplied by the user of the Init Collision Finder.
 */
using GetNewSpec = std::function<boost::optional<DexMethodSpec>(
    const DexMethod*, std::vector<DexType*>*)>;

std::vector<DexType*> find(const Scope& scope, const GetNewSpec& get_new_spec) {
  // Compute what the new prototypes will be after we convert a method. Check
  // the prototypes against existing methods and other prototypes created by
  // this method.
  std::vector<DexType*> result;
  for (const DexClass* cls : scope) {
    std::unordered_set<DexMethodSpec> new_specs;
    for (const DexMethod* m : cls->get_dmethods()) {
      if (method::is_init(m)) {
        std::vector<DexType*> unsafe_refs;
        const auto& new_spec = get_new_spec(m, &unsafe_refs);
        if (new_spec) {
          const auto& pair = new_specs.emplace(*new_spec);
          bool already_there = !pair.second;
          if (already_there || DexMethod::get_method(*new_spec)) {
            always_assert_log(
                !unsafe_refs.empty(),
                "unsafe_refs should be filled with the types that will be "
                "replaced on this <init> method's prototype");
            result.insert(result.end(), unsafe_refs.begin(), unsafe_refs.end());
          }
        }
      }
    }
  }
  return result;
}

} // namespace init_collision_finder
