/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "DexAccess.h"
#include "ReachableClasses.h"
#include <list>

constexpr int kMaxIdentChar (52);

// Type for the map of descriptor -> [newname -> oldname]
// This map is used for reverse lookup to find naming collisions
typedef std::unordered_map<std::string,
    std::unordered_map<std::string, std::string>> NameMapping;

// Renames a field in the Dex
void rename_field(DexField* field, const std::string& new_name);
void rename_method(DexMethod* method, const std::string& new_name);

/*
 * Allows us to wrap Dex elements with a new name that we intend to assign them
 * T should be a pointer to a Dex element (e.g. DexField*)
 */
template <class T>
class DexNameWrapper {
private:
  T dex_elem;
  // the new name that we're trying to give this element
  bool has_new_name{false};
  std::string name{"INVALID_DEFAULT_NAME"};

public:
  // Default constructor is only ever used for map template to work correctly
  // we have added asserts to make sure there are no nullptrs returned.
  DexNameWrapper() = default;
  virtual ~DexNameWrapper() { }
  DexNameWrapper(DexNameWrapper&& other) = default;
  DexNameWrapper(DexNameWrapper const& other) = default;
  // This is the constructor that should be used, creates a new wrapper on
  // the pointer it is passed
  explicit DexNameWrapper(T dex_elem) : dex_elem(dex_elem) { }

  DexNameWrapper& operator=(DexNameWrapper const& other) = default;
  DexNameWrapper& operator=(DexNameWrapper&& other) = default;

  inline T get() { always_assert(dex_elem != nullptr); return dex_elem; }
  inline const T get() const { always_assert(dex_elem != nullptr); return dex_elem; }

  inline bool name_has_changed() const { return has_new_name; }

  const char* get_name() const {
    return has_new_name ? this->name.c_str() : this->get()->get_name()->c_str();
  }

  void set_name(const std::string& new_name) {
    has_new_name = true;
    name = new_name;
  }
};

using FieldNameWrapper = DexNameWrapper<DexField*>;
using DexMethodWrapper = DexNameWrapper<DexMethod*>;

// Will add more to this class later to deal with overriding and interfaces
class MethodNameWrapper : public DexMethodWrapper {
public:
  MethodNameWrapper() = default;
  explicit MethodNameWrapper(DexMethod* dex_elem) :
    DexNameWrapper<DexMethod*>(dex_elem) { }
};

/*
 * Interface for factories for new obfuscated names.
 */
template <class T>
class NameGenerator {
protected:
  int ctr{1};
  inline char get_ident(int num) {
    return num >= 26 ? 'a' + num - 26 : 'A' + num;
  }
  // Set of ids to avoid (these ids were marked as do not rename and we cannot
  // conflict with)
  const std::unordered_set<std::string>& ids_to_avoid;
  // Set of ids we used while assigning names
  std::unordered_set<std::string>& used_ids;
  // Gets the next name that is not in the used_ids set
  std::string next_name() {
    std::string res = "";
    do {
      int ctr_cpy = ctr;
      res.clear();
      while (ctr_cpy > 0) {
        res += get_ident(ctr_cpy % kMaxIdentChar);
        ctr_cpy /= kMaxIdentChar;
      }
      ctr += 1;
      TRACE(OBFUSCATE, 4, "NameGenerator looking for a name, trying: %s\n",
          res.c_str());
    } while (ids_to_avoid.count(res) > 0 || used_ids.count(res) > 0);
    return res;
  }

public:
  NameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>& used_ids) :
      ids_to_avoid(ids_to_avoid), used_ids(used_ids) {}
  virtual ~NameGenerator() = default;

  // We want to rename the DexField pointed to by this wrapper.
  // The new name will be recorded in the wrapper
  virtual void find_new_name(DexNameWrapper<T>& wrap) = 0;

  // Function for saying when we're done figuring out which things we want
  // renamed and we can start actually renaming things
  virtual void bind_names() {}

  virtual void reset() {
    TRACE(OBFUSCATE, 3, "Resetting generator\n");
    ctr = 1;
  }
};

// Simple name generators just immediately bind the next name to the element
// provided.
template <class T>
class SimpleNameGenerator : public NameGenerator<T> {
public:
  SimpleNameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>& used_ids) :
    NameGenerator<T>(ids_to_avoid, used_ids) {}

  void find_new_name(DexNameWrapper<T>& wrap) override {
    std::string new_name(this->next_name());
    wrap.set_name(new_name);
    this->used_ids.insert(new_name);
    T elem = wrap.get();
    TRACE(OBFUSCATE, 2, "\tIntending to rename elem %s (%s) to %s\n",
        SHOW(elem), SHOW(elem->get_name()), new_name.c_str());
  }

  static SimpleNameGenerator<T>* new_name_gen(
      const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>& used_ids) {
    return new SimpleNameGenerator<T>(ids_to_avoid, used_ids);
  }
};

// Will collect all the wrappers of fields to rename then rename them all at
// once making sure to put static final fields with a nullptr value at the end
// (for proper writing of dexes)
class StaticFieldNameGenerator : public NameGenerator<DexField*> {
private:
  std::unordered_set<FieldNameWrapper*> fields;
  std::unordered_set<FieldNameWrapper*> static_final_null_fields;
public:
  StaticFieldNameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>& used_ids) :
    NameGenerator(ids_to_avoid, used_ids) {}

  void find_new_name (FieldNameWrapper& wrap) override {
    DexField* field = wrap.get();
    if (field->get_static_value() == nullptr) {
      static_final_null_fields.insert(&wrap);
    } else {
      fields.insert(&wrap);
    }
  }

  void bind_names() override {
    // Do simple renaming if possible
    if (static_final_null_fields.size() == 0) {
      for (auto wrap : fields) {
        std::string new_name(this->next_name());
        wrap->set_name(new_name);
        this->used_ids.insert(new_name);
      }
      return;
    }
    // Otherwise we have to make sure that all the names are assigned in order
    // such that the static final null fields are at the end
    std::vector<std::string> names;
    for (unsigned int i = 0;
        i < fields.size() + static_final_null_fields.size(); ++i)
      names.emplace_back(this->next_name());
    // if we have more than 52 names, next_name won't be in alphabetical order
    // because it'll return "AA" after "z"
    std::sort(names.begin(), names.end());
    TRACE(OBFUSCATE, 3, "Static Generator\n");
    unsigned int i = 0;
    for (FieldNameWrapper* wrap : fields) {
      const std::string& new_name(names[i++]);
      wrap->set_name(new_name);
      this->used_ids.insert(new_name);
      DexField* field = wrap->get();
      TRACE(OBFUSCATE, 3, "\tIntending to rename field (%s) %s:%s to %s\n",
          SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), new_name.c_str());
    }
    for (FieldNameWrapper* wrap : static_final_null_fields) {
      const std::string& new_name(names[i++]);
      wrap->set_name(new_name);
      this->used_ids.insert(new_name);
      DexField* field = wrap->get();
      TRACE(OBFUSCATE, 3,
          "\tIntending to rename static null field (%s) %s:%s to %s\n",
          SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), new_name.c_str());
    }
  }

  void reset() override {
    NameGenerator<DexField*>::reset();
    fields.clear();
    static_final_null_fields.clear();
  }
};

// T - DexField/DexElem - what we're managing
// R - DexFieldRef/DexElemRef - the ref to what we're managing
// K - DexType/DexProto - the type of the key we're using in the map
template <class T, class R, class K>
class DexElemManager {
protected:
  // Map from class_name -> type -> old_name ->
  //   DexNameWrapper (contains new name)
  std::unordered_map<DexType*,
      std::unordered_map<K,
          std::unordered_map<DexString*, DexNameWrapper<T>>>> elements;
  using RefCtrFn = std::function<R(const std::string&)>;
  using SigGetFn = std::function<K(T&)>;
  using ElemCtrFn = std::function<DexNameWrapper<T>(T&)>;
  SigGetFn sig_getter_fn;
  RefCtrFn ref_getter_fn;
  ElemCtrFn elemCtr;

public:
  DexElemManager(ElemCtrFn elem_ctr, SigGetFn get_sig, RefCtrFn ref_ctr) :
      sig_getter_fn(get_sig),
      ref_getter_fn(ref_ctr),
      elemCtr(elem_ctr) { }
  DexElemManager(DexElemManager&&) = default;
  virtual ~DexElemManager() {}

  inline bool contains_elem(
      DexType* cls, K sig, DexString* name) {
    return elements.count(cls) > 0 &&
      elements[cls].count(sig) > 0 &&
      elements[cls][sig].count(name) > 0;
  }

  inline bool contains_elem(T& elem) {
    return contains_elem(
        elem->get_class(), sig_getter_fn(elem), elem->get_name());
  }

  inline DexNameWrapper<T>& emplace(T& elem) {
    elements[elem->get_class()][sig_getter_fn(elem)][elem->get_name()] =
        elemCtr(elem);
    return elements[elem->get_class()][sig_getter_fn(elem)][elem->get_name()];
  }

  // Mirrors the map get operator, but ensures we create correct wrappers
  // if they don't exist
  inline DexNameWrapper<T>& operator[](T& elem) {
    return contains_elem(elem) ?
      elements[elem->get_class()]
        [sig_getter_fn(elem)][elem->get_name()] : emplace(elem);
  }

  // Commits all the renamings in elements to the dex by modifying the
  // underlying DexFields. Does in-place modification.
  void commit_renamings_to_dex() {
    for (const auto& class_itr : this->elements) {
      for (const auto& type_itr : class_itr.second) {
        for (const auto& name_wrap : type_itr.second) {
          const auto& wrap = name_wrap.second;
          if (!wrap.name_has_changed()) continue;
          auto elem = wrap.get();
          always_assert_log(should_rename_elem(elem),
              "Trying to rename (%s) %s:%s to %s, but we shouldn't\n",
              SHOW(sig_getter_fn(elem)), SHOW(elem->get_class()), SHOW(elem),
              wrap.get_name());
          TRACE(OBFUSCATE, 2,
              "\tRenaming the elem 0x%x (%s) %s:%s to %s\n",
              elem, SHOW(sig_getter_fn(elem)), SHOW(elem->get_class()),
              SHOW(elem->get_name()), wrap.get_name());

          elem->change(ref_getter_fn(wrap.get_name()));
        }
      }
    }
  }

  // Does a lookup over the fields we renamed in the dex to see what the
  // reference should be reset with. Returns kSouldNotReset if there is no
  // mapping.
  // Note: we also have to look in superclasses in the case that this is a ref
  T def_of_ref(T ref) {
    DexType* cls = ref->get_class();
    while (type_class(cls) && !type_class(cls)->is_external()) {
      if (contains_elem(cls, sig_getter_fn(ref), ref->get_name())) {
        DexNameWrapper<T>& wrap(
          elements[cls][sig_getter_fn(ref)][ref->get_name()]);
        if (wrap.name_has_changed())
          return wrap.get();
      }
      cls = type_class(cls)->get_super_class();
    }
    return nullptr;
  }

  // Debug print of the mapping
  void print_elements() {
    TRACE(OBFUSCATE, 4, "Elem Ptr: (type/proto) class:old name -> new name\n");
    for (const auto& class_itr : elements) {
      for (const auto& type_itr : class_itr.second) {
        for (const auto& name_wrap : type_itr.second) {
          auto elem = name_wrap.second.get();
          TRACE(OBFUSCATE, 4, " 0x%x: (%s) %s:%s -> %s\n",
              elem,
              SHOW(type_itr.first),
              SHOW(class_itr.first),
              SHOW(name_wrap.first),
              name_wrap.second.get_name());
        }
      }
    }
  }
};

typedef DexElemManager<DexField*, DexFieldRef, DexType*> DexFieldManager;
DexFieldManager new_dex_field_manager();
typedef DexElemManager<DexMethod*, DexMethodRef, DexProto*> DexMethodManager;
DexMethodManager new_dex_method_manager();

// Whether or not the configs allow for us to obfuscate the member
// We don't want to obfuscate seeds. Keep members shouldn't be obfuscated
// unless we are explicitly told to do so with the allowobfuscation flag
// an element being a seed trumps allowobfuscation.
template <class T>
bool should_rename_elem(const T member) {
  return !is_seed(member) && (!keep(member) || allowobfuscation(member));
}

// Look at a list of members and check if there is a renamable member in the list
template <class T>
bool contains_renamable_elem(const std::list<T>& elems) {
  for (T e : elems)
    if (should_rename_elem(e))
      return true;
  return false;
}

enum HierarchyDirection {
  VisitNeither      = 0,
  VisitSuperClasses = (1<<0),
  VisitSubClasses   = (1<<1),
  VisitBoth         = VisitSuperClasses | VisitSubClasses };

// Walks the class hierarchy starting at this class and including
// superclasses (including external ones) and/or subclasses based on
// the specified HierarchyDirection.
// T - type of element we're visiting
// Visitor - the lambda to call on each element
// get_list - the function to get a list of elements to operate on from a class
template <class T, typename Visitor>
void walk_hierarchy(
    DexClass* cls,
    std::function<std::list<T>&(DexClass*)> get_list,
    Visitor on_member,
    HierarchyDirection h_dir) {
  if (!cls) return;
  auto visit = [&](DexClass* cls) {
    if (!cls->is_external())
      for (auto& elem : get_list(cls))
        on_member(elem);
  };
  visit(cls);

  // TODO: revisit for methods to be careful around Object
  if (h_dir & HierarchyDirection::VisitSuperClasses) {
    auto clazz = cls;
    while (clazz) {
      visit(clazz);
      if (clazz->get_super_class() == nullptr) break;
      clazz = type_class(clazz->get_super_class());
    }
  }

  if (h_dir & HierarchyDirection::VisitSubClasses) {
    for (auto subcls_type : get_children(cls->get_type())) {
      walk_hierarchy(type_class(subcls_type), get_list, on_member,
          HierarchyDirection::VisitSubClasses);
    }
  }
}

// Static state of the renamer (wrapper for args for obfuscation)
template <class T>
class RenamingContext {
public:
  const std::list<T>& elems;
  const std::unordered_set<std::string>& ids_to_avoid;
  const bool operateOnPrivates;
  NameGenerator<T>& name_gen;

  RenamingContext(std::list<T>& elems,
      std::unordered_set<std::string>& ids_to_avoid,
      NameGenerator<T>& name_gen,
      bool operateOnPrivates) :
      elems(elems), ids_to_avoid(ids_to_avoid),
      operateOnPrivates(operateOnPrivates), name_gen(name_gen) { }
  virtual ~RenamingContext() {}

  // Whether or not on this pass we should rename the member
  virtual bool can_rename_elem(T elem) const {
    return should_rename_elem(elem) && operateOnPrivates == is_private(elem);
  }

};

typedef RenamingContext<DexField*> FieldRenamingContext;

// Method renaming context is special because we have to make sure we don't
// rename <init> or <clinit> ever regardless of configs
class MethodRenamingContext : public RenamingContext<DexMethod*> {
  DexString* initstr = DexString::get_string("<init>");
  DexString* clinitstr = DexString::get_string("<clinit>");
public:
  MethodRenamingContext(std::list<DexMethod*>& elems,
      std::unordered_set<std::string>& ids_to_avoid,
      NameGenerator<DexMethod*>& name_gen,
      bool operateOnPrivates) : RenamingContext<DexMethod*>(
          elems, ids_to_avoid, name_gen, operateOnPrivates) { }

  // For methods we have to make sure we don't rename <init> or <clinit> ever
  virtual bool can_rename_elem(DexMethod* elem) const override {
    return should_rename_elem(elem) && operateOnPrivates == is_private(elem) &&
      elem->get_name() != initstr && elem->get_name() != clinitstr;
  }
};

// State of the renaming that we need to modify as we rename more fields
template <class T, class R, class K>
class ObfuscationState {
public:
  // Ids that we've used in renaming
  std::unordered_set<std::string> used_ids;
  // Ids to avoid in renaming
  std::unordered_set<std::string> ids_to_avoid;

  virtual ~ObfuscationState() = default;

  virtual void populate_ids_to_avoid(DexClass* base,
      DexElemManager<T, R, K>& name_manager, bool visitSubclasses) = 0;
};

class FieldObfuscationState :
    public ObfuscationState<DexField*, DexFieldRef, DexType*> {
public:
  void populate_ids_to_avoid(DexClass* base,
      DexFieldManager& name_manager, bool /* unused */) override {
    for (auto f : base->get_ifields())
      ids_to_avoid.insert(name_manager[f].get_name());
    for (auto f : base->get_sfields())
      ids_to_avoid.insert(name_manager[f].get_name());
  }
};

class MethodObfuscationState :
    public ObfuscationState<DexMethod*, DexMethodRef, DexProto*> {
public:
  // Essentially what this does is walks the hierarchy collecting names of
  // public methods in superclasses and any methods in this class (and
  // subclasses if visitSubclasses is specified)
  void populate_ids_to_avoid(DexClass* base,
      DexMethodManager& name_manager, bool visitSubclasses) override {
    std::function<std::list<DexMethod*>&(DexClass*)> get_demthods =
      [](DexClass* c) -> std::list<DexMethod*>& { return c->get_dmethods(); };
    std::function<std::list<DexMethod*>&(DexClass*)> get_vmethods =
      [](DexClass* c) -> std::list<DexMethod*>& { return c->get_vmethods(); };
    // If a method has a new name, we want to add both the new name and
    // the previous name
    auto insert_name =
      [&](DexMethod* m) {
        auto& wrap(name_manager[m]);
        if (wrap.name_has_changed())
          ids_to_avoid.insert(SHOW(wrap.get()->get_name()));
        ids_to_avoid.insert(wrap.get_name()); };
    auto insert_if_private =
      [&](DexMethod* m) { if(!is_private(base)) insert_name(m); };
    walk_hierarchy(base, get_demthods, insert_if_private,
      HierarchyDirection::VisitSuperClasses);
    walk_hierarchy(base, get_demthods, insert_name,
      visitSubclasses ? HierarchyDirection::VisitSubClasses :
        HierarchyDirection::VisitNeither);
    walk_hierarchy(base, get_vmethods, insert_if_private,
      HierarchyDirection::VisitSuperClasses);
    walk_hierarchy(base, get_vmethods, insert_name,
      visitSubclasses ? HierarchyDirection::VisitSubClasses :
        HierarchyDirection::VisitNeither);
  }
};
