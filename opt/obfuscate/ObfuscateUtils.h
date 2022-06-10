/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ClassHierarchy.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "Trace.h"

#include <list>
#include <type_traits>

namespace obfuscate_utils {
void compute_identifier(int value, std::string* res);
} // namespace obfuscate_utils

// Type for the map of descriptor -> [newname -> oldname]
// This map is used for reverse lookup to find naming collisions
using NameMapping =
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>>;

// Renames a field in the Dex
void rename_field(DexField* field, const std::string& new_name);
void rename_method(DexMethod* method, const std::string& new_name);

// Whether or not the configs allow for us to obfuscate the member
// We don't want to obfuscate seeds. Keep members shouldn't be obfuscated
// unless we are explicitly told to do so with the allowobfuscation flag
// an element being a seed trumps allowobfuscation.
template <class T>
bool should_rename_elem(const T* member) {
  auto cls = type_class(member->get_class());
  return can_rename(member) && !member->is_external() && cls != nullptr &&
         !cls->is_external();
}

/*
 * Allows us to wrap Dex elements with a new name that we intend to assign them
 * T should be a pointer to a Dex element (e.g. DexField*). We cannot just
 * assign names as we create them because of collisions and issues around
 * vmethods (requires some additional information). Additionally, some record
 * of the old name is necessary to fix up ref opcodes.
 */
template <class T,
          typename std::enable_if<std::is_pointer<T>::value, int>::type = 0>
class DexNameWrapper {
 protected:
  T dex_elem;
  // the new name that we're trying to give this element
  bool has_new_name{false};
  std::string name{"INVALID_DEFAULT_NAME"};

 public:
  using const_type = typename std::add_const<T>::type;

  // Default constructor is only ever used for map template to work correctly
  // we have added asserts to make sure there are no nullptrs returned.
  DexNameWrapper() = default;
  virtual ~DexNameWrapper() {}
  DexNameWrapper(DexNameWrapper&& other) noexcept = default;
  DexNameWrapper(DexNameWrapper const& other) = default;
  // This is the constructor that should be used, creates a new wrapper on
  // the pointer it is passed
  explicit DexNameWrapper(T dex_elem) : dex_elem(dex_elem) {}

  DexNameWrapper& operator=(DexNameWrapper const& other) = default;
  DexNameWrapper& operator=(DexNameWrapper&& other) noexcept = default;

  inline T get() {
    always_assert(dex_elem != nullptr);
    return dex_elem;
  }
  inline const_type get() const {
    always_assert(dex_elem != nullptr);
    return dex_elem;
  }

  virtual bool name_has_changed() { return has_new_name; }

  virtual const char* get_name() {
    return has_new_name ? this->name.c_str() : this->get()->get_name()->c_str();
  }

  virtual void set_name(const std::string& new_name) {
    has_new_name = true;
    name = new_name;
    // Uncomment this line for debug renaming
    // name = std::string(SHOW(dex_elem->get_name())) + "_" + new_name;
  }

  // Meant to be overridden, but we don't want this class to be abstract
  virtual void mark_unrenamable() { not_reached(); }
  virtual void mark_renamable() { not_reached(); }
  virtual bool should_rename() { not_reached(); }

  virtual std::string get_printable() {
    std::ostringstream res;
    res << "  0x" << this->get() << ": " << show(dex_elem) << " -> "
        << get_name();
    return res.str();
  }

  bool is_modified() {
    return this->name_has_changed() && this->should_rename();
  }

  virtual bool should_commit() {
    return !type_class(get()->get_class())->is_external();
  }
};

using DexFieldWrapper = DexNameWrapper<DexField*>;
using DexMethodWrapper = DexNameWrapper<DexMethod*>;

class FieldNameWrapper : public DexFieldWrapper {
 public:
  FieldNameWrapper() = default;
  explicit FieldNameWrapper(DexField* elem) : DexFieldWrapper(elem) {}

  bool should_rename() override { return true; }
};

class MethodNameWrapper : public DexMethodWrapper {
 private:
  int n_links = 1;
  // Allows us to link wrappers together to show groups of wrappers that
  // have to be renamed together. If there is a non-null next pointer, any calls
  // looking for information will be forwarded to the last element in the chain
  MethodNameWrapper* next;
  bool renamable = true;

  // Updates our links union-find style so that finding the end of the chain
  // is cheap
  inline void update_link() {
    if (next != nullptr) {
      next->update_link();
      next = find_end_link();
    }
  }

  MethodNameWrapper* find_end_link() {
    if (next == nullptr) return this;
    return next->find_end_link();
  }

 public:
  MethodNameWrapper() = default;
  explicit MethodNameWrapper(DexMethod* dex_elem)
      : DexNameWrapper<DexMethod*>(dex_elem), next(nullptr) {
    // Make sure on construction any external elements are unrenamable
    renamable = should_rename_elem(dex_elem);
  }

  bool name_has_changed() override {
    if (next == nullptr) return has_new_name;
    update_link();
    return next->name_has_changed();
  }

  const char* get_name() override {
    if (next == nullptr) return DexMethodWrapper::get_name();
    update_link();
    return next->get_name();
  }

  void set_name(const std::string& new_name) override {
    if (next == nullptr) {
      DexMethodWrapper::set_name(new_name);
      return;
    }
    next->set_name(new_name);
    update_link();
  }

  void link(MethodNameWrapper* other) {
    always_assert(other != nullptr);
    always_assert(other != this);
    always_assert(
        strcmp(SHOW(get()->get_name()), SHOW(other->get()->get_name())) == 0);

    auto this_end = find_end_link();
    // Make sure they aren't already linked
    if (this_end == other->find_end_link()) return;

    if (next == nullptr) {
      next = other;
      // Make sure if either isn't renamable, we mark the end result as not
      // renamable
      if (!renamable || !other->should_rename()) {
        TRACE(OBFUSCATE, 4, "Elem %s marking\n\t%s unrenamable",
              SHOW(renamable ? other->get() : this->get()),
              SHOW(renamable ? this->get() : other->get()));
        other->mark_unrenamable();
      }
    } else {
      this_end->next = other;
      other->n_links += this_end->n_links;
      update_link();
    }
  }

  int get_num_links() {
    update_link();
    return find_end_link()->n_links;
  }

  bool should_rename() override {
    if (next == nullptr) return renamable;
    update_link();
    return next->should_rename();
  }

  bool is_linked() { return next != nullptr; }

  void mark_unrenamable() override {
    if (next == nullptr) {
      TRACE(OBFUSCATE, 4, "Elem %s marked unrenamable", SHOW(this->get()));
      renamable = false;
      return;
    }
    update_link();
    next->mark_unrenamable();
  }

  bool should_commit() override { return true; }

  std::string get_printable() override {
    std::ostringstream res;
    res << "  0x" << this << ": " << show(dex_elem) << " -> " << get_name()
        << " => 0x" << next;
    return res.str();
  }
};

/*
 * Interface for factories for new obfuscated names.
 */
template <class T>
class NameGenerator {
 protected:
  int ctr{0};

  // Set of ids to avoid (these ids were marked as do not rename and we cannot
  // conflict with)
  const std::unordered_set<std::string>& ids_to_avoid;
  // Set of ids we used while assigning names
  std::unordered_set<std::string>& used_ids;
  // Gets the next name that is not in the used_ids set
  std::string next_name() {
    std::string res;
    do {
      res.clear();
      obfuscate_utils::compute_identifier(ctr++, &res);
      TRACE(OBFUSCATE, 4, "NameGenerator looking for a name, trying: %s",
            res.c_str());
    } while (ids_to_avoid.count(res) > 0 || used_ids.count(res) > 0);
    return res;
  }

 public:
  NameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
                std::unordered_set<std::string>& used_ids)
      : ids_to_avoid(ids_to_avoid), used_ids(used_ids) {}
  virtual ~NameGenerator() = default;

  // We want to rename the DexField pointed to by this wrapper.
  // The new name will be recorded in the wrapper
  virtual void find_new_name(DexNameWrapper<T>* wrap) = 0;

  // Function for saying when we're done figuring out which things we want
  // renamed and we can start actually renaming things
  virtual void bind_names() {}

  int next_ctr() { return ctr; }
};

// We define various "biased" comparators: We re-order fields and methods
// to place those which are "similar" close to each other, e.g. sharing the
// same static value/proto/type/access.
// This increases compressibility of associated metadata structures.
// The re-ordering happens before renaming, after which all tables are again in
// sorted-by-renamed-names order, while the underlying items got reshuffled.
struct proto_access_biased_dexmethods_comparator {
  bool operator()(const DexMethod* a, const DexMethod* b) const {
    // First checking proto, then access yields the biggest compressed wins for
    // many apps.
    if (a->get_proto() != b->get_proto()) {
      return compare_dexprotos(a->get_proto(), b->get_proto());
    }
    if (a->get_access() != b->get_access()) {
      return a->get_access() < b->get_access();
    }
    return compare_dexmethods(a, b);
  }
};

class MethodNameGenerator : public NameGenerator<DexMethod*> {
 private:
  // Use ordered maps here to get deterministic names, and to sort for best
  // compressibility
  std::map<DexMethod*,
           DexMethodWrapper*,
           proto_access_biased_dexmethods_comparator>
      methods;

 public:
  MethodNameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
                      std::unordered_set<std::string>& used_ids)
      : NameGenerator<DexMethod*>(ids_to_avoid, used_ids) {}

  void find_new_name(DexMethodWrapper* wrap) override {
    DexMethod* method = wrap->get();
    methods.emplace(method, wrap);
  }

  void bind_names() override {
    for (auto p : methods) {
      auto wrap = p.second;
      always_assert(!wrap->is_modified());
      do {
        std::string new_name(this->next_name());
        wrap->set_name(new_name);
        this->used_ids.insert(new_name);
        TRACE(OBFUSCATE, 3, "\tTrying method name %s for %s", wrap->get_name(),
              SHOW(wrap->get()));
      } while (DexMethod::get_method(wrap->get()->get_class(),
                                     DexString::make_string(wrap->get_name()),
                                     wrap->get()->get_proto()) != nullptr);
      // Keep spinning on a name until you find one that isn't used at all
      TRACE(OBFUSCATE, 2,
            "\tIntending to rename method %s (%s) to %s ids to avoid %zu",
            SHOW(wrap->get()), SHOW(wrap->get()->get_name()), wrap->get_name(),
            ids_to_avoid.size());
    }
  }
};

// We sort static fields according to their static values. This reduces the
// number of needed encoded static values for static fields, and it increases
// the compressibility of associated metadata structures.
struct static_value_biased_dexfields_comparator {
  bool operator()(const DexField* a, const DexField* b) const {
    // We prefer instance fields over static fields
    if (is_static(a) != is_static(b)) {
      return !is_static(a);
    }

    auto eva = a->get_static_value();
    auto evb = b->get_static_value();
    auto is_eva_relevant = eva && !eva->is_zero();
    auto is_evb_relevant = evb && !evb->is_zero();
    always_assert(!is_eva_relevant || is_static(a));
    always_assert(!is_evb_relevant || is_static(b));
    // We prefer fields that have relevant static values
    if (is_eva_relevant != is_evb_relevant) {
      return is_eva_relevant;
    }
    if (is_eva_relevant) {
      // We are biasing the comparator to the static value --- its type,
      // and then its actual value. If the type/value is indistinguishtable,
      // we still fall back to a more basic comparator at the very end of this
      // function, so that different fields are still properly distinguished.
      if (eva->evtype() != evb->evtype()) {
        return eva->evtype() < evb->evtype();
      }
      switch (eva->evtype()) {
      case DEVT_STRING: {
        auto evastring = static_cast<DexEncodedValueString*>(eva)->string();
        auto evbstring = static_cast<DexEncodedValueString*>(evb)->string();
        if (evastring != evbstring) {
          return compare_dexstrings(evastring, evbstring);
        }
        break;
      }
      case DEVT_TYPE: {
        auto evatype = static_cast<DexEncodedValueType*>(eva)->type();
        auto evbtype = static_cast<DexEncodedValueType*>(evb)->type();
        if (evatype != evbtype) {
          return compare_dextypes(evatype, evbtype);
        }
        break;
      }
      case DEVT_FIELD: {
        auto evafield = static_cast<DexEncodedValueField*>(eva)->field();
        auto evbfield = static_cast<DexEncodedValueField*>(evb)->field();
        if (evafield != evbfield) {
          return compare_dexfields(evafield, evbfield);
        }
        break;
      }
      case DEVT_METHOD: {
        auto evamethod = static_cast<DexEncodedValueMethod*>(eva)->method();
        auto evbmethod = static_cast<DexEncodedValueMethod*>(evb)->method();
        if (evamethod != evbmethod) {
          return compare_dexmethods(evamethod, evbmethod);
        }
        break;
      }
      case DEVT_ARRAY: {
        auto evaarray = static_cast<DexEncodedValueArray*>(eva);
        auto evbarray = static_cast<DexEncodedValueArray*>(evb);
        if (evaarray->is_static_val() != evbarray->is_static_val()) {
          return evaarray->is_static_val() < evbarray->is_static_val();
        }
        auto evavalues = evaarray->evalues();
        auto evbvalues = evbarray->evalues();
        if (evavalues->size() != evbvalues->size()) {
          return evavalues->size() < evbvalues->size();
        }
        // TODO: deep-inspect array, but likely little impact
        break;
      }
      case DEVT_ANNOTATION:
        // TODO: deep-inspect this, but doesn't seem to occur in practice
        break;
      default:
        if (eva->value() != evb->value()) {
          return eva->value() < evb->value();
        }
      }
    }

    // Now we are comparing fields, ignoring any static values.
    // First checking access, then type yields the biggest compressed wins for
    // many apps.
    if (a->get_access() != b->get_access()) {
      return a->get_access() < b->get_access();
    }
    if (a->get_type() != b->get_type()) {
      return compare_dextypes(a->get_type(), b->get_type());
    }
    return compare_dexfields(a, b);
  }
};

// Will collect all the wrappers of fields to rename then rename them all at
// once making sure to put static final fields with a nullptr value at the end
// (for proper writing of dexes)
class FieldNameGenerator : public NameGenerator<DexField*> {
 private:
  // Use ordered maps here to get deterministic names, and to sort for best
  // compressibility
  std::
      map<DexField*, DexFieldWrapper*, static_value_biased_dexfields_comparator>
          fields;

 public:
  FieldNameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
                     std::unordered_set<std::string>& used_ids)
      : NameGenerator<DexField*>(ids_to_avoid, used_ids) {}

  void find_new_name(DexFieldWrapper* wrap) override {
    DexField* field = wrap->get();
    fields.emplace(field, wrap);
  }

  void bind_names() override {
    for (auto p : fields) {
      auto wrap = p.second;
      always_assert(!wrap->is_modified());
      do {
        std::string new_name(this->next_name());
        wrap->set_name(new_name);
        this->used_ids.insert(new_name);
        TRACE(OBFUSCATE, 2, "\tTrying field name %s for %s", wrap->get_name(),
              SHOW(wrap->get()));
      } while (DexField::get_field(wrap->get()->get_class(),
                                   DexString::make_string(wrap->get_name()),
                                   wrap->get()->get_type()) != nullptr);
      // Keep spinning on a name until you find one that isn't used at all
      TRACE(OBFUSCATE,
            2,
            "\tIntending to rename elem %s (%s) (renamable %s) to %s",
            SHOW(wrap->get()),
            SHOW(wrap->get()->get_name()),
            should_rename_elem(wrap->get()) ? "true" : "false",
            wrap->get_name());
    }
  }
};

// T - DexField/DexElem - what we're managing
// R - DexFieldRef/DexElemRef - the ref to what we're managing
// S - DexFieldSpec/DexElemSpec - the spec to what we're managing
// K - DexType/DexProto - the type of the key we're using in the map
template <class T, class R, class S, class K>
class DexElemManager {
 protected:
  // Map from class_name -> type -> old_name ->
  //   DexNameWrapper (contains new name)
  // Note: unique_ptr necessary here to avoid object slicing
  std::unordered_map<
      DexType*,
      std::unordered_map<
          K,
          std::unordered_map<const DexString*,
                             std::unique_ptr<DexNameWrapper<T>>>>>
      elements;
  using RefCtrFn = std::function<S(const std::string&)>;
  using SigGetFn = std::function<K(R)>;
  using ElemCtrFn = std::function<DexNameWrapper<T>*(T&)>;
  SigGetFn sig_getter_fn;
  RefCtrFn ref_getter_fn;
  ElemCtrFn elemCtr;

  bool mark_all_unrenamable;

 public:
  DexElemManager(ElemCtrFn elem_ctr, SigGetFn get_sig, RefCtrFn ref_ctr)
      : sig_getter_fn(get_sig),
        ref_getter_fn(ref_ctr),
        elemCtr(elem_ctr),
        mark_all_unrenamable(false) {}
  // NOLINTNEXTLINE(performance-noexcept-move-constructor)
  DexElemManager(DexElemManager&& other) = default;
  virtual ~DexElemManager() {}

  // void lock_elements() { mark_all_unrenamable = true; }
  // void unlock_elements() { mark_all_unrenamable = false; }

  inline bool contains_elem(DexType* cls, K sig, const DexString* name) {
    return elements.count(cls) > 0 && elements[cls].count(sig) > 0 &&
           elements[cls][sig].count(name) > 0;
  }

  inline bool contains_elem(R elem) {
    return contains_elem(elem->get_class(), sig_getter_fn(elem),
                         elem->get_name());
  }

  inline DexNameWrapper<T>* emplace(T elem) {
    elements[elem->get_class()][sig_getter_fn(elem)][elem->get_name()] =
        std::unique_ptr<DexNameWrapper<T>>(elemCtr(elem));
    if (mark_all_unrenamable)
      elements[elem->get_class()][sig_getter_fn(elem)][elem->get_name()]
          ->mark_unrenamable();
    return elements[elem->get_class()][sig_getter_fn(elem)][elem->get_name()]
        .get();
  }

  // Mirrors the map get operator, but ensures we create correct wrappers
  // if they don't exist
  inline DexNameWrapper<T>* operator[](T elem) {
    return contains_elem(elem) ? elements[elem->get_class()]
                                         [sig_getter_fn(elem)][elem->get_name()]
                                             .get()
                               : emplace(elem);
  }

  // Commits all the renamings in elements to the dex by modifying the
  // underlying DexFields. Does in-place modification. Returns the number
  // of elements renamed
  int commit_renamings_to_dex() {
    std::unordered_set<T> renamed_elems;
    int renamings = 0;
    for (auto& class_itr : this->elements) {
      for (auto& type_itr : class_itr.second) {
        for (auto& name_wrap : type_itr.second) {
          auto& wrap = name_wrap.second;
          // need both because of methods ??
          if (!wrap->is_modified() || !should_rename_elem(wrap->get()) ||
              !wrap->should_commit() ||
              strcmp(wrap->get()->get_name()->c_str(), wrap->get_name()) == 0) {
            TRACE(OBFUSCATE, 2, "Not committing %s to %s", SHOW(wrap->get()),
                  wrap->get_name());
            continue;
          }
          auto elem = wrap->get();
          TRACE(OBFUSCATE, 2,
                "\tRenaming the elem 0x%p %s%s to %s external: %s can_rename: "
                "%s\n",
                elem, SHOW(sig_getter_fn(elem)), SHOW(elem), wrap->get_name(),
                type_class(elem->get_class())->is_external() ? "true" : "false",
                can_rename(elem) ? "true" : "false");
          if (renamed_elems.count(elem) > 0) {
            TRACE(OBFUSCATE, 2, "Found elem we've already renamed %s",
                  SHOW(elem));
          }
          renamed_elems.insert(elem);
          elem->change(ref_getter_fn(wrap->get_name()),
                       false /* rename on collision */);
          renamings++;
        }
      }
    }
    return renamings;
  }

 private:
  // Returns the def for that class and ref if it exists, nullptr otherwise
  T find_def(R ref, DexType* cls) {
    if (cls == nullptr) return nullptr;
    if (contains_elem(cls, sig_getter_fn(ref), ref->get_name())) {
      DexNameWrapper<T>* wrap =
          elements[cls][sig_getter_fn(ref)][ref->get_name()].get();
      if (wrap->is_modified()) return wrap->get();
    }
    return nullptr;
  }

  /**
   * Look up in the class and all its interfaces.
   */
  T find_def_in_class_and_intf(R ref, DexClass* cls) {
    if (cls == nullptr) return nullptr;
    auto found_def = find_def(ref, cls->get_type());
    if (found_def != nullptr) return found_def;
    for (auto& intf : cls->get_interfaces()->get_type_list()) {
      auto found = find_def_in_class_and_intf(ref, type_class(intf));
      if (found != nullptr) return found;
    }
    return nullptr;
  }

 public:
  // Does a lookup over the fields we renamed in the dex to see what the
  // reference should be reset with. Returns nullptr if there is no mapping.
  // Note: we also have to look in superclasses in the case that this is a ref
  T def_of_ref(R ref) {
    DexClass* cls = type_class(ref->get_class());
    while (cls && !cls->is_external()) {
      auto found = find_def_in_class_and_intf(ref, cls);
      if (found) {
        return found;
      }
      cls = type_class(cls->get_super_class());
    }
    return nullptr;
  }

  // Debug print of the mapping
  virtual void print_elements() {
    TRACE(OBFUSCATE, 4, "Elem Ptr: (type/proto) class:old name -> new name");
    for (auto& class_itr : elements) {
      for (auto& type_itr : class_itr.second) {
        for (auto& name_wrap : type_itr.second) {
          TRACE(OBFUSCATE, 2, " (%s) %s", SHOW(type_itr.first),
                name_wrap.second->get_printable().c_str());
          /*SHOW(class_itr.first),
          SHOW(name_wrap.first),
          name_wrap.second->get_name());*/
        }
      }
    }
  }
};

using DexFieldManager =
    DexElemManager<DexField*, DexFieldRef*, DexFieldSpec, DexType*>;
DexFieldManager new_dex_field_manager();
using DexMethodManager =
    DexElemManager<DexMethod*, DexMethodRef*, DexMethodSpec, DexProto*>;
DexMethodManager new_dex_method_manager();

// Look at a list of members and check if there is a renamable member
template <class T, class R, class S, class K>
bool contains_renamable_elem(const std::vector<T>& elems,
                             DexElemManager<T, R, S, K>& name_mapping) {
  for (T e : elems)
    if (should_rename_elem(e) && !name_mapping[e]->name_has_changed() &&
        name_mapping[e]->should_rename())
      return true;
  return false;
}

// Static state of the renamer (wrapper for args for obfuscation)
template <class T>
class RenamingContext {
 public:
  const std::vector<T>& elems;
  NameGenerator<T>& name_gen;

  RenamingContext(std::vector<T>& elems, NameGenerator<T>& name_gen)
      : elems(elems), name_gen(name_gen) {}
  virtual ~RenamingContext() {}

  // Whether or not on this pass we should rename the member
  virtual bool can_rename_elem(T elem) const { return can_rename(elem); }
};

using FieldRenamingContext = RenamingContext<DexField*>;

// Method renaming context is special because we have to make sure we don't
// rename <init> or <clinit> ever regardless of configs
class MethodRenamingContext : public RenamingContext<DexMethod*> {
  const DexString* initstr = DexString::get_string("<init>");
  const DexString* clinitstr = DexString::get_string("<clinit>");
  DexMethodManager& name_mapping;

 public:
  MethodRenamingContext(std::vector<DexMethod*>& elems,
                        NameGenerator<DexMethod*>& name_gen,
                        DexMethodManager& name_mapping)
      : RenamingContext<DexMethod*>(elems, name_gen),
        name_mapping(name_mapping) {}

  // For methods we have to make sure we don't rename <init> or <clinit> ever
  bool can_rename_elem(DexMethod* elem) const override {
    return should_rename_elem(elem) && elem->get_name() != initstr &&
           elem->get_name() != clinitstr && name_mapping[elem]->should_rename();
  }
};

// State of the renaming that we need to modify as we rename more fields
template <class T, class R, class S, class K>
class ObfuscationState {
 public:
  // Ids that we've used in renaming
  std::unordered_set<std::string> used_ids;
  // Ids to avoid in renaming
  std::unordered_set<std::string> ids_to_avoid;

  virtual ~ObfuscationState() = default;

  virtual void populate_ids_to_avoid(DexClass* base,
                                     DexElemManager<T, R, S, K>& name_manager,
                                     const ClassHierarchy& ch) = 0;
};

class FieldObfuscationState
    : public ObfuscationState<DexField*, DexFieldRef*, DexFieldSpec, DexType*> {
 public:
  void populate_ids_to_avoid(DexClass* base,
                             DexFieldManager& name_manager,
                             const ClassHierarchy& /* unused */) override {
    for (auto f : base->get_ifields())
      ids_to_avoid.insert(name_manager[f]->get_name());
    for (auto f : base->get_sfields())
      ids_to_avoid.insert(name_manager[f]->get_name());
  }
};

enum HierarchyDirection {
  VisitNeither = 0,
  VisitSuperClasses = (1 << 0),
  VisitSubClasses = (1 << 1),
  VisitBoth = VisitSuperClasses | VisitSubClasses
};

// Walks the class hierarchy starting at this class and including
// superclasses (including external ones) and/or subclasses based on
// the specified HierarchyDirection.
// T - type of element we're visiting
// Visitor - the lambda to call on each element
// get_list - the function to get a list of elements to operate on from a class
template <typename Visitor>
void walk_hierarchy(DexClass* cls,
                    Visitor on_member,
                    bool visit_private,
                    HierarchyDirection h_dir,
                    const ClassHierarchy& ch) {
  if (!cls) return;
  auto visit = [&](DexClass* cls) {
    for (const auto meth : const_cast<const DexClass*>(cls)->get_dmethods()) {
      if (!is_private(meth) || visit_private)
        on_member(const_cast<DexMethod*>(meth));
    }

    for (const auto meth : const_cast<const DexClass*>(cls)->get_vmethods()) {
      if (!is_private(meth) || visit_private)
        on_member(const_cast<DexMethod*>(meth));
    }
  };

  visit(cls);

  // TODO: revisit for methods to be careful around Object
  // We shouldn't need to visit object because we shouldn't ever be renaming
  // to the name of a java.lang.Object method
  if (h_dir & HierarchyDirection::VisitSuperClasses) {
    auto clazz = cls;
    while (clazz) {
      visit(clazz);
      if (clazz->get_super_class() == nullptr) break;
      clazz = type_class(clazz->get_super_class());
    }
  }

  if (h_dir & HierarchyDirection::VisitSubClasses) {
    for (auto subcls_type : get_children(ch, cls->get_type())) {
      walk_hierarchy(type_class(subcls_type), on_member, visit_private,
                     HierarchyDirection::VisitSubClasses, ch);
    }
  }
}

class MethodObfuscationState : public ObfuscationState<DexMethod*,
                                                       DexMethodRef*,
                                                       DexMethodSpec,
                                                       DexProto*> {
 public:
  // Essentially what this does is walks the hierarchy collecting names of
  // public methods in superclasses and any methods in this class (and
  // subclasses if visitSubclasses is specified)
  void populate_ids_to_avoid(DexClass* base,
                             DexMethodManager& name_manager,
                             const ClassHierarchy& ch) override {
    auto visit_member = [&](DexMethod* m) {
      auto wrap(name_manager[m]);
      if (wrap->name_has_changed())
        ids_to_avoid.insert(SHOW(wrap->get()->get_name()));
      ids_to_avoid.insert(wrap->get_name());
    };
    walk_hierarchy(base, visit_member, false,
                   HierarchyDirection::VisitSuperClasses, ch);
    walk_hierarchy(base, visit_member, true,
                   HierarchyDirection::VisitSubClasses, ch);
  }
};
