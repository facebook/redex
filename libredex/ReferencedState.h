/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

class ReferencedState {
 private:
  bool m_bytype{false};
  bool m_bystring{false};
  /* m_computed is a "clear-only" flag;  If one of the reflects is
   * non-computed, all subsequents should be non-computed.
   * Reflect marking which is computed from code means that
   * it can/should be recomputed periodically when doing
   * optimizations.  For instance, deleting a method with
   * a reflection target will then allow that reflection
   * target to be re-evaluated.
   */
  bool m_computed{true};
  bool m_seed{false};
  bool m_renamed_seed{false};

  // ProGuard keep settings
  bool m_keep{false}; // Specify classes and class members that are entry-points
  bool m_keepclassmembers{false}; // Specify member to be preserved if the
                                  // class is preserved.
  bool m_keepclasseswithmembers{false}; // Specify that all classes with the
                                        // given members should be specified.

 public:
  ReferencedState() = default;
  bool can_delete() const { return !m_bytype; }
  bool can_rename() const { return !m_bystring; }

  /**
   * Is this item a "seed" according to ProGuard's analysis?
   */
  bool is_seed() const { return m_seed; }

  /**
   * Is this item a "seed" when we take into account renaming done by ProGuard?
   * We don't always want to consider renamed seeds; using this flag will give
   * more conservative results than `is_seed()`
   */
  bool is_renamed_seed() const { return m_renamed_seed; }

  // ProGuard option
  bool keep() const { return m_keep; }
  bool keepclassmembers() const { return m_keepclassmembers; }
  bool keepclasseswithmembers() const { return m_keepclasseswithmembers; }

  // For example, a classname in a layout, e.g. <com.facebook.MyCustomView />
  // is a ref_by_string with from_code = false
  //
  // Class c = Class.forName("com.facebook.FooBar");
  // is a ref_by_string with from_code = true
  void ref_by_string(bool from_code) {
    m_bytype = m_bystring = true;
    m_computed = m_computed && from_code;
  }

  // A direct reference from code (not reflection)
  void ref_by_type() { m_bytype = true; }

  /* Called before recompute */
  void clear_if_compute() {
    if (m_computed) {
      m_bytype = m_bystring = false;
    }
  }

  // A class marked to be kept from the list of seeds from ProGuard
  void ref_by_seed() { m_seed = true; }

  // Mark item to be kept, even if it's been renamed by ProGuard
  void ref_by_renamed_seed() { m_renamed_seed = true; }

  // ProGuaurd keep information.
  void set_keep() { m_keep = true; }

  void set_keepclassmembers() { m_keepclassmembers = true; }

  void set_keepclasseswithmembers() { m_keepclasseswithmembers = true; }
};
