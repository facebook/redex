/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <boost/optional.hpp>
#include <limits>
#include <mutex>
#include <string>

#include "Debug.h"
#include "KeepReason.h"

namespace ir_meta_io {
class IRMetaIO;
} // namespace ir_meta_io

namespace keep_rules {
namespace impl {
class KeepState;
} // namespace impl
} // namespace keep_rules

using InterdexSubgroupIdx = uint32_t;

class ReferencedState {
 private:
  struct InnerStruct {
    int8_t m_api_level{-1};

    // Whether this DexMember is referenced by one of the strings in the native
    // libraries. Note that this doesn't allow us to distinguish
    // native -> Java references from Java -> native refs.
    bool m_by_string : 1;
    // Whether it is referenced from an XML layout.
    bool m_by_resources : 1;
    // Whether it is a json serializer/deserializer class for a reachable class.
    bool m_is_serde : 1;

    // ProGuard keep settings
    //
    // Whether any keep rule has matched this. This applies for both `-keep` and
    // `-keepnames`.
    bool m_keep : 1;
    // assumenosideeffects allows certain methods to be removed.
    bool m_assumenosideeffects : 1;
    // If m_whyareyoukeeping is true then report debugging information
    // about why this class or member is being kept.
    bool m_whyareyoukeeping : 1;

    // For keep modifiers: -keep,allowshrinking and -keep,allowobfuscation.
    //
    // Instead of m_allowshrinking and m_allowobfuscation, we need to have
    // set/unset pairs for easier parallelization. The unset has a high
    // priority. See the comments in apply_keep_modifiers.
    bool m_set_allowshrinking : 1;
    bool m_unset_allowshrinking : 1;
    bool m_set_allowobfuscation : 1;
    bool m_unset_allowobfuscation : 1;

    // For keep modifier: -keep,includedescriptorclasses
    bool m_includedescriptorclasses : 1;

    bool m_no_optimizations : 1;

    bool m_generated : 1;

    // For inlining configurations.
    bool m_dont_inline : 1;
    bool m_force_inline : 1;

    // This is set by the ImmutableGetters pass. It indicates that a method
    // is pure.
    bool m_immutable_getter : 1;

    // This is set by the AnalyzePureMethodsPass. It indicates that a method
    // is pure as defined in Purity.h.
    // *WARNING*
    // Any optimisation that might alter the semantics in such a way that it is
    // no longer pure should invalidate this flag. It could also be invalidated
    // by running AnalyzePureMethodsPass which would recompute it.
    bool m_pure_method : 1;

    // Whether this member is an outlined class or method.
    bool m_outlined : 1;

    // Is this member is a kotlin class or method
    bool m_is_kotlin : 1;

    bool m_name_used : 1;

    // Whether a field is used to indicate that an sget cannot be removed
    // because it signals that the class must be initialized at this point.
    bool m_init_class : 1;

    // This may be set on classes, indicating that its static initializer has no
    // side effects.
    bool m_clinit_has_no_side_effects : 1;

    InnerStruct() {
      // Initializers in bit fields are C++20...
      m_by_string = false;
      m_by_resources = false;
      m_is_serde = false;

      m_keep = false;
      m_assumenosideeffects = false;
      m_whyareyoukeeping = false;

      m_set_allowshrinking = false;
      m_unset_allowshrinking = false;
      m_set_allowobfuscation = false;
      m_unset_allowobfuscation = false;

      m_includedescriptorclasses = false;

      m_no_optimizations = false;

      m_generated = false;

      m_dont_inline = false;
      m_force_inline = false;

      m_immutable_getter = false;
      m_pure_method = false;
      m_outlined = false;
      m_is_kotlin = false;

      m_name_used = false;

      m_init_class = false;
      m_clinit_has_no_side_effects = false;
    }
  } inner_struct;

  // InterDex subgroup, if any.
  // NOTE: Will be set ONLY for generated classes.
  static constexpr InterdexSubgroupIdx kNoSubgroup =
      std::numeric_limits<InterdexSubgroupIdx>::max();
  InterdexSubgroupIdx m_interdex_subgroup{kNoSubgroup};

  // Going through hoops here to reduce the size of ReferencedState while
  // keeping memory requirements still small in non-default case.
  struct KeepReasons {
    std::mutex m_keep_reasons_mtx;
    keep_reason::ReasonPtrSet m_keep_reasons;
  };
  mutable std::atomic<KeepReasons*> m_keep_reasons{nullptr};

 public:
  ReferencedState() = default;
  ReferencedState(const ReferencedState&) = delete;
  ~ReferencedState() { delete m_keep_reasons.load(); }

  ReferencedState& operator=(const ReferencedState& other) {
    if (this != &other) {
      this->inner_struct = other.inner_struct;
      if (other.m_keep_reasons.load() != nullptr) {
        auto& k = ensure_keep_reasons();
        k.m_keep_reasons = other.m_keep_reasons.load()->m_keep_reasons;
      }
    }
    return *this;
  }

  void join_with(const ReferencedState& other) {
    if (this != &other) {
      this->inner_struct.m_by_string =
          this->inner_struct.m_by_string | other.inner_struct.m_by_string;
      this->inner_struct.m_by_resources =
          this->inner_struct.m_by_resources | other.inner_struct.m_by_resources;
      this->inner_struct.m_is_serde =
          this->inner_struct.m_is_serde | other.inner_struct.m_is_serde;

      this->inner_struct.m_keep =
          this->inner_struct.m_keep | other.inner_struct.m_keep;
      this->inner_struct.m_assumenosideeffects =
          this->inner_struct.m_assumenosideeffects &
          other.inner_struct.m_assumenosideeffects;
      this->inner_struct.m_whyareyoukeeping =
          this->inner_struct.m_whyareyoukeeping |
          other.inner_struct.m_whyareyoukeeping;

      this->inner_struct.m_set_allowshrinking =
          this->inner_struct.m_set_allowshrinking &
          other.inner_struct.m_set_allowshrinking;
      this->inner_struct.m_unset_allowshrinking =
          this->inner_struct.m_unset_allowshrinking |
          other.inner_struct.m_unset_allowshrinking;
      this->inner_struct.m_set_allowobfuscation =
          this->inner_struct.m_set_allowobfuscation &
          other.inner_struct.m_set_allowobfuscation;
      this->inner_struct.m_unset_allowobfuscation =
          this->inner_struct.m_unset_allowobfuscation |
          other.inner_struct.m_unset_allowobfuscation;

      this->inner_struct.m_includedescriptorclasses =
          this->inner_struct.m_includedescriptorclasses |
          other.inner_struct.m_includedescriptorclasses;

      this->inner_struct.m_no_optimizations =
          this->inner_struct.m_no_optimizations |
          other.inner_struct.m_no_optimizations;
      this->inner_struct.m_dont_inline =
          this->inner_struct.m_dont_inline | other.inner_struct.m_dont_inline;
      this->inner_struct.m_force_inline =
          this->inner_struct.m_force_inline & other.inner_struct.m_force_inline;

      this->inner_struct.m_immutable_getter =
          this->inner_struct.m_immutable_getter &
          other.inner_struct.m_immutable_getter;
      this->inner_struct.m_pure_method =
          this->inner_struct.m_pure_method & other.inner_struct.m_pure_method;
      this->inner_struct.m_is_kotlin =
          this->inner_struct.m_is_kotlin & other.inner_struct.m_is_kotlin;
      this->inner_struct.m_outlined =
          this->inner_struct.m_outlined & other.inner_struct.m_outlined;

      this->inner_struct.m_init_class =
          this->inner_struct.m_init_class | other.inner_struct.m_init_class;
    }
  }

  std::string str() const;

  // ProGuard keep options

  // -keep
  bool can_delete() const {
    return (!inner_struct.m_keep || allowshrinking()) &&
           !inner_struct.m_by_resources;
  }

  // -keepnames
  bool can_rename() const {
    return can_rename_if_also_renaming_xml() && !inner_struct.m_by_resources &&
           !inner_struct.m_name_used;
  }

  /*
   * Returns whether :member can be renamed if references to it from XML
   * resources are also updated accordingly. The optimizing pass in question
   * will be responsible for updating the XML resources.
   */
  bool can_rename_if_also_renaming_xml() const {
    return (!inner_struct.m_name_used && !inner_struct.m_keep) ||
           allowobfuscation();
  }

  bool assumenosideeffects() const {
    return inner_struct.m_assumenosideeffects;
  }

  bool report_whyareyoukeeping() const {
    return inner_struct.m_whyareyoukeeping;
  }

  // For example, a classname in a layout, e.g. <com.facebook.MyCustomView /> or
  // Class c = Class.forName("com.facebook.FooBar");
  void ref_by_string() { inner_struct.m_by_string = true; }

  bool is_referenced_by_string() const { return inner_struct.m_by_string; }

  // A class referenced by resource XML can take the following forms in .xml
  // files under the res/ directory:
  // <com.facebook.FooView />
  // <fragment android:name="com.facebook.BarFragment" />
  //
  // This differs from "by_string" reference since it is possible to rename
  // these string references, and potentially eliminate dead resource .xml files
  void set_referenced_by_resource_xml() {
    inner_struct.m_by_resources = true;
    if (keep_reason::Reason::record_keep_reasons()) {
      add_keep_reason(keep_reason::Reason::make_keep_reason(keep_reason::XML));
    }
  }

  void unset_referenced_by_resource_xml() {
    inner_struct.m_by_resources = false;
    // TODO: Remove the XML-related keep reasons
  }

  bool is_referenced_by_resource_xml() const {
    return inner_struct.m_by_resources;
  }

  void set_is_serde() { inner_struct.m_is_serde = true; }

  bool is_serde() const { return inner_struct.m_is_serde; }

  void set_root() { set_root(keep_reason::UNKNOWN); }

  /*
   * Mark this DexMember as an entry point that should not be deleted or
   * renamed.
   *
   * The ...args are arguments to the keep_reason::Reason constructor.
   * The typical Redex run does not care to keep the extra diagnostic
   * information of the keep reasons, so it seems worthwhile to forward these
   * arguments to avoid the construction of unused Reason objects when
   * record_keep_reasons() is false.
   */
  template <class... Args>
  void set_root(Args&&... args) {
    inner_struct.m_keep = true;
    unset_allowshrinking();
    unset_allowobfuscation();
    if (keep_reason::Reason::record_keep_reasons()) {
      add_keep_reason(
          keep_reason::Reason::make_keep_reason(std::forward<Args>(args)...));
    }
  }

  void unset_root() {
    inner_struct.m_keep = false;
    inner_struct.m_unset_allowshrinking = false;
    inner_struct.m_unset_allowobfuscation = false;
  }

  const keep_reason::ReasonPtrSet& keep_reasons() const {
    if (!keep_reason::Reason::record_keep_reasons()) {
      // We really should not allow this.
      static keep_reason::ReasonPtrSet SINGLETON;
      return SINGLETON;
    }
    auto& keep_reasons = ensure_keep_reasons();
    return keep_reasons.m_keep_reasons;
  }

  template <class... Args>
  void set_keepnames(Args&&... args) {
    set_has_keep(std::forward<Args>(args)...);
    set_allowshrinking();
    unset_allowobfuscation();
  }

  void set_keepnames() { set_keepnames(keep_reason::UNKNOWN); }

  // This one should only be used by UnmarkProguardKeepPass to unmark proguard
  // keep rule after proguard file processing is finished. Because
  // ProguardMatcher uses parallel processing, using this will result in race
  // condition.
  void force_unset_allowshrinking() {
    inner_struct.m_set_allowshrinking = true;
    inner_struct.m_unset_allowshrinking = false;
  }

  void set_assumenosideeffects() { inner_struct.m_assumenosideeffects = true; }

  void set_whyareyoukeeping() { inner_struct.m_whyareyoukeeping = true; }

  void set_interdex_subgroup(
      const boost::optional<InterdexSubgroupIdx>& interdex_subgroup) {
    m_interdex_subgroup = interdex_subgroup ? *interdex_subgroup : kNoSubgroup;
  }
  InterdexSubgroupIdx get_interdex_subgroup() const {
    return m_interdex_subgroup;
  }
  bool has_interdex_subgroup() const {
    return m_interdex_subgroup != kNoSubgroup;
  }

  // -1 means unknown, e.g. for a method created by Redex
  int8_t get_api_level() const { return inner_struct.m_api_level; }
  void set_api_level(int32_t api_level) {
    redex_assert(api_level <= std::numeric_limits<int8_t>::max());
    inner_struct.m_api_level = api_level;
  }

  bool no_optimizations() const { return inner_struct.m_no_optimizations; }
  void set_no_optimizations() { inner_struct.m_no_optimizations = true; }
  void reset_no_optimizations() { inner_struct.m_no_optimizations = false; }

  // Methods and classes marked as "generated" tend to not have stable names,
  // and don't properly participate in coldstart tracking.
  bool is_generated() const { return inner_struct.m_generated; }
  void set_generated() { inner_struct.m_generated = true; }

  bool force_inline() const { return inner_struct.m_force_inline; }
  void set_force_inline() { inner_struct.m_force_inline = true; }
  bool dont_inline() const { return inner_struct.m_dont_inline; }
  void set_dont_inline() { inner_struct.m_dont_inline = true; }

  bool immutable_getter() const { return inner_struct.m_immutable_getter; }
  void set_immutable_getter() { inner_struct.m_immutable_getter = true; }

  bool pure_method() const { return inner_struct.m_pure_method; }
  void set_pure_method() { inner_struct.m_pure_method = true; }
  void reset_pure_method() { inner_struct.m_pure_method = false; }

  bool outlined() const { return inner_struct.m_outlined; }
  void set_outlined() { inner_struct.m_outlined = true; }
  void reset_outlined() { inner_struct.m_outlined = false; }
  bool is_cls_kotlin() const { return inner_struct.m_is_kotlin; }
  void set_cls_kotlin() { inner_struct.m_is_kotlin = true; }
  void set_name_used() { inner_struct.m_name_used = true; }
  bool name_used() { return inner_struct.m_name_used; }

  bool init_class() const { return inner_struct.m_init_class; }
  void set_init_class() { inner_struct.m_init_class = true; }
  void set_clinit_has_no_side_effects() {
    inner_struct.m_clinit_has_no_side_effects = true;
  }
  bool clinit_has_no_side_effects() const {
    return inner_struct.m_clinit_has_no_side_effects;
  }

 private:
  // Does any keep rule (whether -keep or -keepnames) match this DexMember?
  bool has_keep() const { return inner_struct.m_keep; }

  /*
   * This should only be called from ProguardMatcher, and is used whenever we
   * encounter a keep rule (regardless of whether it's `-keep` or `-keepnames`).
   */
  template <class... Args>
  void set_has_keep(Args&&... args) {
    inner_struct.m_keep = true;
    if (keep_reason::Reason::record_keep_reasons()) {
      add_keep_reason(
          keep_reason::Reason::make_keep_reason(std::forward<Args>(args)...));
    }
  }

  // There's generally no need to call this; use can_delete() instead.
  bool allowshrinking() const {
    return !inner_struct.m_unset_allowshrinking &&
           inner_struct.m_set_allowshrinking;
  }

  void set_allowshrinking() { inner_struct.m_set_allowshrinking = true; }

  void unset_allowshrinking() { inner_struct.m_unset_allowshrinking = true; }

  // There's generally no need to call this; use can_rename() instead.
  bool allowobfuscation() const {
    return !inner_struct.m_unset_allowobfuscation &&
           inner_struct.m_set_allowobfuscation;
  }

  void set_allowobfuscation() { inner_struct.m_set_allowobfuscation = true; }

  void unset_allowobfuscation() {
    inner_struct.m_unset_allowobfuscation = true;
  }

  bool includedescriptorclasses() const {
    return inner_struct.m_includedescriptorclasses;
  }

  void set_includedescriptorclasses() {
    inner_struct.m_includedescriptorclasses = true;
  }

  KeepReasons& ensure_keep_reasons() const {
    always_assert(keep_reason::Reason::record_keep_reasons());
    // First see whether it's already done to avoid allocating.
    auto attempt = m_keep_reasons.load();
    if (attempt != nullptr) {
      return *attempt;
    }
    // OK, allocate and CAS.
    auto keep_reasons = std::make_unique<KeepReasons>();
    KeepReasons* expected = nullptr;
    if (m_keep_reasons.compare_exchange_strong(expected, keep_reasons.get())) {
      return *keep_reasons.release();
    }
    return *expected;
  }

  void add_keep_reason(const keep_reason::Reason* reason) {
    always_assert(keep_reason::Reason::record_keep_reasons());
    auto& keep_reasons = ensure_keep_reasons();
    std::lock_guard<std::mutex> lock(keep_reasons.m_keep_reasons_mtx);
    keep_reasons.m_keep_reasons.emplace(reason);
  }

  friend class keep_rules::impl::KeepState;

  // IR serialization class
  friend class ir_meta_io::IRMetaIO;
};
