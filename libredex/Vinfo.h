/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

#include <unordered_map>
#include <unordered_set>

/**
 * Vinfo is a helper/ancillary data structure which can be built on-demand
 * and is used to answer questions about virtual methods, e.g. is a vmethod
 * overridden, what does a vmethod override, where was a vmethod originally
 * declared, etc.
 *
 * This data structure should be rebuilt whenever changes occur to the type
 * hierarchy with mutation of classes/methods, e.g. DelSuper can delete
 * vmethods.
 *
 * The following caveats apply to ALL methods on Vinfo and will not be
 * reiterated in each piece of method documentation.
 *
 * - You may only query using concrete/resolved methods.
 * - Interfaces are totally ignored. You may not query using a DexMethod
 *   from an interface nor will you ever get back an answer that takes
 *   interfaces into account. A class method which implements an interface
 *   method will not be seen as "overriding" anything.
 * - Abstract methods are considered proper methods. If a subclass re-abstracts
 *   a super class method (which can happen in java), the abstract method is
 *   seen as an override.
 *
 */
class Vinfo {
 public:
  using methods_t = std::unordered_set<const DexMethod*>;
  struct vinfo_t {
    const DexMethod* decl = nullptr;
    const DexMethod* override_of = nullptr;
    bool is_overriden = false;
    methods_t overriden_by;
  };
  using vinfos_t = std::unordered_map<const DexMethod*, vinfo_t>;

  explicit Vinfo(const std::vector<DexClass*>& scope);

  /**
   * Finds the topmost declaration of this method.
   *
   * @param meth The method to query. Must be concrete.
   * @return Topmost declaration, **possibly including meth itself**
   */
  const DexMethod* get_decl(const DexMethod* meth);

  /**
   * Determines whether or not the given method overrides another method.
   * Implementing an interface method is not considered an override.
   *
   * @param meth The method to query. Must be concrete.
   * @return true iff meth overrides another method defined higher in the
   *              inheritance hierarchy.
   */
  bool is_override(const DexMethod* meth);

  /**
   * Get the method which meth overrides. This will always be the *nearest*
   * override (e.g. most specific generalization).
   *
   * @param meth The method to query. Must be concrete.
   * @return The method which this overrides. Will only be a method on a
   *         class, not an interface. This returns null IFF is_override
   *         returns false for 'meth'
   */
  const DexMethod* get_overriden_method(const DexMethod* meth);

  /**
   * Determines if this method is overriden by any subclasses.
   *
   * @param meth The method to query. Must be concrete.
   * @return true if this method is overriden in any subclasses of the class
   *         on which it's defined.
   */
  bool is_overriden(const DexMethod* meth);

  /**
   * Get the methods which *directly* override meth, e.g. first-order
   * overrides, but not not overrides of overrides.
   *
   * @param meth The method to query. Must be concrete.
   * @return An unordered set of the override methods (may be empty, of course)
   */
  const methods_t& get_override_methods(const DexMethod* meth);

 private:
  vinfos_t m_vinfos;
};
