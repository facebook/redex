/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <string>

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

  // ProGuard keep settings
  // Specify classes and class members that are entry-points.
  bool m_keep{false};
  // Useful for keeping native methods.
  bool m_includedescriptorclasses{false};
  // Specify items that can be deleted.
  bool m_allowshrinking{false};
  // Not used by the Redex ProGuard rule matcher.
  bool m_allowoptimization{false};
  // Not used by the Redex ProGuard rule matcher.
  bool m_allowobfuscation{false};
  // assumenosideeffects allows certain methods to be removed.
  bool m_assumenosideeffects{false};
  // Does this class have a blanket keep,allowshrinking applied to it?
  bool m_blanket_keep{false};
  // The number of keep rules that touch this class.
  unsigned int m_keep_count{0};
  // If m_whyareyoukeeping is true then report debugging information
  // about why this class or member is being kept.
  bool m_whyareyoukeeping{false};
  bool m_keep_name{false};

 public:
  ReferencedState() = default;

  std::string str() const;

  bool can_delete() const { return !m_bytype && (!m_keep || m_allowshrinking); }
  bool can_rename() const {
    return !m_keep_name && !m_bystring && (!m_keep || m_allowobfuscation) &&
           !m_allowshrinking;
  }

  /**
   * Is this item a "seed" according to ProGuard's analysis?
   */
  bool is_seed() const { return m_seed; }

  // ProGuard keep options
  bool keep() const { return m_keep; }
  // ProGaurd keep option modifiers
  bool includedescriptorclasses() const { return m_includedescriptorclasses; }
  bool allowshrinking() const { return m_allowshrinking; }
  bool allowoptimization() const { return m_allowoptimization; }
  bool allowobfuscation() const { return m_allowobfuscation; }
  bool assumenosideeffects() const { return m_assumenosideeffects; }

  bool is_blanket_kept() const { return m_blanket_keep && m_keep_count == 1; }

  bool report_whyareyoukeeping() const { return m_whyareyoukeeping; }

  // For example, a classname in a layout, e.g. <com.facebook.MyCustomView />
  // is a ref_by_string with from_code = false
  //
  // Class c = Class.forName("com.facebook.FooBar");
  // is a ref_by_string with from_code = true
  void ref_by_string(bool from_code) {
    m_bytype = m_bystring = true;
    m_computed = m_computed && from_code;
  }

  bool is_referenced_by_string() { return m_bystring; }

  // A direct reference from code (not reflection)
  void ref_by_type() { m_bytype = true; }

  bool is_referenced_by_type() { return m_bytype; }

  /* Called before recompute */
  void clear_if_compute() {
    if (m_computed) {
      m_bytype = m_bystring = false;
    }
  }

  // A class marked to be kept from the list of seeds from ProGuard
  void ref_by_seed() { m_seed = true; }

  // ProGuard keep information.
  void set_keep() { m_keep = true; }

  void set_keep_name() { m_keep_name = true; }

  void set_includedescriptorclasses() { m_includedescriptorclasses = true; }

  void set_allowshrinking() { m_allowshrinking = true; }
  void unset_allowshrinking() { m_allowshrinking = false; }

  void set_allowoptimization() { m_allowoptimization = true; }

  void set_allowobfuscation() { m_allowobfuscation = true; }
  void unset_allowobfuscation() { m_allowobfuscation = false; }

  void set_assumenosideeffects() { m_assumenosideeffects = true; }

  void set_blanket_keep() { m_blanket_keep = true; }

  void increment_keep_count() { m_keep_count++; }

  void set_whyareyoukeeping() { m_whyareyoukeeping = true; }
};
