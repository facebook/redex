/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <atomic>
#include <string>

class ReferencedState {
 private:
  bool m_bytype{false};
  bool m_bystring{false};
  bool m_byresources{false};

  // ProGuard keep settings
  //
  // Specify classes and class members that are entry-points.
  bool m_keep{false};
  // assumenosideeffects allows certain methods to be removed.
  bool m_assumenosideeffects{false};
  // Does this class have a blanket "-keepnames class *" applied to it?
  // "-keepnames" is synonym with "-keep,allowshrinking".
  bool m_blanket_keepnames{false};
  // If m_whyareyoukeeping is true then report debugging information
  // about why this class or member is being kept.
  bool m_whyareyoukeeping{false};

  // For keep modifiers: -keep,allowshrinking and -keep,allowobfuscation.
  //
  // Instead of m_allowshrinking and m_allowobfuscation, we need to have
  // set/unset pairs for easier parallelization. The unset has a high priority.
  // See the comments in apply_keep_modifiers.
  bool m_set_allowshrinking{false};
  bool m_unset_allowshrinking{false};
  bool m_set_allowobfuscation{false};
  bool m_unset_allowobfuscation{false};

  bool m_keep_name{false};

  // The number of keep rules that touch this class.
  std::atomic<unsigned int> m_keep_count{0};

 public:
  ReferencedState() = default;

  // std::atomic requires an explicitly user-defined assignment operator.
  ReferencedState& operator=(const ReferencedState& other) {
    if (this != &other) {
      this->m_bytype = other.m_bytype;
      this->m_bystring = other.m_bystring;
      this->m_byresources = other.m_byresources;

      this->m_keep = other.m_keep;
      this->m_assumenosideeffects = other.m_assumenosideeffects;
      this->m_blanket_keepnames = other.m_blanket_keepnames;
      this->m_whyareyoukeeping = other.m_whyareyoukeeping;

      this->m_set_allowshrinking = other.m_set_allowshrinking;
      this->m_unset_allowshrinking = other.m_unset_allowshrinking;
      this->m_set_allowobfuscation = other.m_set_allowobfuscation;
      this->m_unset_allowobfuscation = other.m_unset_allowobfuscation;

      this->m_keep_name = other.m_keep_name;

      this->m_keep_count = other.m_keep_count.load();
    }
    return *this;
  }

  std::string str() const;

  bool can_delete() const {
    return !m_bytype && !m_byresources && (!m_keep || allowshrinking());
  }
  bool can_rename() const {
    return !m_keep_name && !m_bystring && (!m_keep || allowobfuscation()) &&
           !allowshrinking();
  }

  // ProGuard keep options
  bool keep() const { return m_keep || m_byresources; }

  // ProGaurd keep option modifiers
  bool allowshrinking() const {
    return !m_unset_allowshrinking && m_set_allowshrinking;
  }
  bool allowobfuscation() const {
    return !m_unset_allowobfuscation && m_set_allowobfuscation;
  }
  bool assumenosideeffects() const { return m_assumenosideeffects; }

  bool is_blanket_names_kept() const {
    return m_blanket_keepnames && m_keep_count == 1;
  }

  bool report_whyareyoukeeping() const { return m_whyareyoukeeping; }

  // For example, a classname in a layout, e.g. <com.facebook.MyCustomView /> or
  // Class c = Class.forName("com.facebook.FooBar");
  void ref_by_string() {
    m_bytype = m_bystring = true;
  }
  bool is_referenced_by_string() const { return m_bystring; }

  // A class referenced by resource XML can take the following forms in .xml
  // files under the res/ directory:
  // <com.facebook.FooView />
  // <fragmnet android:name="com.facebook.BarFragment" />
  //
  // This differs from "by_string" reference since it is possible to rename
  // these string references, and potentially eliminate dead resource .xml files
  void set_referenced_by_resource_xml() { m_byresources = true; }
  void unset_referenced_by_resource_xml() { m_byresources = false; }
  bool is_referenced_by_resource_xml() const { return m_byresources; }

  // A direct reference from code (not reflection)
  void ref_by_type() { m_bytype = true; }

  // ProGuard keep information.
  void set_keep() { m_keep = true; }

  void set_keep_name() { m_keep_name = true; }

  void set_allowshrinking() { m_set_allowshrinking = true; }
  void unset_allowshrinking() { m_unset_allowshrinking = true; }

  void set_allowobfuscation() { m_set_allowobfuscation = true; }
  void unset_allowobfuscation() { m_unset_allowobfuscation = true; }

  void set_assumenosideeffects() { m_assumenosideeffects = true; }

  void set_blanket_keepnames() { m_blanket_keepnames = true; }

  void increment_keep_count() { m_keep_count++; }

  void set_whyareyoukeeping() { m_whyareyoukeeping = true; }
};
