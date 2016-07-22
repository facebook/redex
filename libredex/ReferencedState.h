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
  bool m_bytype;
  bool m_bystring;
  /* m_computed is a "clear-only" flag;  If one of the reflects is
   * non-computed, all subsequents should be non-computed.
   * Reflect marking which is computed from code means that
   * it can/should be recomputed periodically when doing
   * optimizations.  For instance, deleting a method with
   * a reflection target will then allow that reflection
   * target to be re-evaluated.
   */
  bool m_computed;
  bool m_seed;

 public:
  ReferencedState() {
    m_bytype = m_bystring = false;
    m_computed = true;
    m_seed = false;
  }
  bool can_delete() const { return !m_bytype; }
  bool can_rename() const { return !m_bystring; }
  bool is_seed() const { return m_seed; }

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
  void ref_by_type() {
    m_bytype = true;
  }

  /* Called before recompute */
  void clear_if_compute() {
    if (m_computed) {
      m_bytype = m_bystring = false;
    }
  }

  // A class marked to be kept from the list of seedds from ProGuard
  void ref_by_seed() {
    m_seed = true;
  }

};
