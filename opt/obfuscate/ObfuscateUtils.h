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
  std::unordered_set<std::string>* used_ids;
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
    } while (ids_to_avoid.count(res) > 0 || used_ids->count(res) > 0);
    return res;
  }

public:
  NameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>* used_ids) :
      ids_to_avoid(ids_to_avoid), used_ids(used_ids) {}
  virtual ~NameGenerator() = default;

  // We want to rename the DexField pointed to by this wrapper.
  // The new name will be recorded in the wrapper
  virtual void find_new_name(DexNameWrapper<T>* wrap) = 0;

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
      std::unordered_set<std::string>* used_ids) :
    NameGenerator<T>(ids_to_avoid, used_ids) {}

  void find_new_name(DexNameWrapper<T>* wrap) override {
    std::string new_name(this->next_name());
    wrap->set_name(new_name);
    this->used_ids->insert(new_name);
    T elem = wrap->get();
    TRACE(OBFUSCATE, 2, "\tIntending to rename elem (%s) %s:%s to %s\n",
        SHOW(elem->get_type()), SHOW(elem->get_class()),
        SHOW(elem->get_name()), new_name.c_str());
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
      std::unordered_set<std::string>* used_ids) :
    NameGenerator(ids_to_avoid, used_ids) {}

  void find_new_name (FieldNameWrapper* wrap) override {
    DexField* field = wrap->get();
    if (field->get_static_value() == nullptr) {
      static_final_null_fields.insert(wrap);
    } else {
      fields.insert(wrap);
    }
  }

  void bind_names() override {
    // Do simple renaming if possible
    if (static_final_null_fields.size() == 0) {
      for (auto wrap : fields) {
        std::string new_name(this->next_name());
        wrap->set_name(new_name);
        this->used_ids->insert(new_name);
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
      this->used_ids->insert(new_name);
      DexField* field = wrap->get();
      TRACE(OBFUSCATE, 3, "\tIntending to rename field (%s) %s:%s to %s\n",
          SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), new_name.c_str());
    }
    for (FieldNameWrapper* wrap : static_final_null_fields) {
      const std::string& new_name(names[i++]);
      wrap->set_name(new_name);
      this->used_ids->insert(new_name);
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

template <class T>
class DexElemManager {
protected:
  // Map from class_name -> type -> old_name ->
  //   DexNameWrapper (contains new name)
  // TODO: modify so that we can create MethodNameWrappers correctly without
  //       object slicing
  std::unordered_map<DexType*,
    std::unordered_map<DexType*,
      std::unordered_map<DexString*, DexNameWrapper<T>>>> elements;

public:
  virtual ~DexElemManager() {}
  inline bool contains_field(
      DexType* cls, DexType* type, DexString* name) {
    return elements.count(cls) > 0 &&
      elements[cls].count(type) > 0 &&
      elements[cls][type].count(name) > 0;
  }

  inline bool contains_field(T& elem) {
    return contains_field(elem->get_class(),
      elem->get_type(), elem->get_name());
  }

  // Mirrors the map get operator, but ensures we create correct wrappers
  // if they don't exist
  DexNameWrapper<T>& operator[](T& elem) {
    return contains_field(elem) ?
      elements[elem->get_class()]
        [elem->get_type()][elem->get_name()] :
      elements[elem->get_class()][elem->get_type()].emplace(
        elem->get_name(), FieldNameWrapper(elem)).first->second;
  }

  virtual void commit_renamings_to_dex() = 0;
  virtual T def_of_ref(T ref) = 0;
  virtual void print_elements() = 0;
};

class DexFieldManager : public DexElemManager<DexField*> {
  // Commits all the renamings in elements to the dex by modifying the
  // underlying DexFields. Does in-place modification.
  void commit_renamings_to_dex() override;

  // Does a lookup over the fields we renamed in the dex to see what the
  // reference should be reset with. Returns kSouldNotReset if there is no
  // mapping.
  // Note: we also have to look in superclasses in the case that this is a ref
  DexField* def_of_ref(DexField* ref) override;

  // Debug print of the mapping
  void print_elements() override;
};

class DexMethodManager {};

class MemberVisitor {
public:
  virtual ~MemberVisitor() = default;
  virtual void visit_field(DexField* /* unused */) { }
  virtual void visit_method(DexMethod* /* unused */) { }
};

// We need to be able to walk the class hierarchy to figure out conflicts
class ClassVisitor {
public:
  enum VisitFilter {
  PrivateOnly      = (1<<0),
  NonPrivateOnly   = (1<<1),
  All              = PrivateOnly | NonPrivateOnly };
protected:
const VisitFilter visit_filter;
MemberVisitor* member_visitor;
public:
ClassVisitor(VisitFilter vf, MemberVisitor* mv) :
    visit_filter(vf), member_visitor(mv) { }
void visit(DexClass*);

inline bool should_visit_private() {
  return visit_filter & VisitFilter::PrivateOnly;
}

inline bool should_visit_public() {
  return visit_filter & VisitFilter::NonPrivateOnly;
}
};

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

// Used to collect all conflicting names
class FieldNameCollector : public MemberVisitor {
private:
  std::unordered_set<std::string>* names;
  DexElemManager<DexField*>* field_name_mapping;

public:
  FieldNameCollector(
      DexElemManager<DexField*>* field_name_mapping,
      std::unordered_set<std::string>* names) :
        names(names), field_name_mapping(field_name_mapping) { }
  virtual ~FieldNameCollector() = default;

  virtual void visit_field(DexField* f) override {
    TRACE(OBFUSCATE, 4, "Visiting field %s\n", SHOW(f));
    names->insert((*field_name_mapping)[f].get_name());
  }
};

// Used to collect all conflicting names
class MethodNameCollector : public MemberVisitor {
private:
  // May want to do map from DexProto* if we want to do overloading aggressively
  std::unordered_set<std::string>* names;
  DexMethodManager* method_name_mapping;

public:
  MethodNameCollector(
      DexMethodManager* method_name_mapping,
      std::unordered_set<std::string>* names) :
        names(names), method_name_mapping(method_name_mapping) { }
  virtual ~MethodNameCollector() = default;

  virtual void visit_method(DexMethod* m) override {
    // TODO: implement after implementing DexMethodManager
    TRACE(OBFUSCATE, 4, "Visiting Method %s\n", SHOW(m));
    //names->insert((*method_name_mapping)[m].get_name());
  }
};

enum HierarchyDirection {
  VisitNeither      = 0,
  VisitSuperClasses = (1<<0),
  VisitSubClasses   = (1<<1),
  VisitBoth         = VisitSuperClasses | VisitSubClasses };
// gets children get_children(dc) map through type_class
void walk_hierarchy(DexClass* cls,
    ClassVisitor* visitor,
    HierarchyDirection h_dir);

// State of the renaming that we need to modify as we rename more members
template <class T>
class ObfuscationState {
  // Cache of ref -> def mapping in case we have many instructions that
  // use the same ref.
  std::unordered_map<T, T> ref_def_cache;
public:
  DexElemManager<T>& name_mapping;
  std::unordered_set<std::string> used_ids;

  ObfuscationState(DexElemManager<T>& name_mapping) : name_mapping(name_mapping) { }

  void commit_renamings_to_dex() {
    name_mapping.commit_renamings_to_dex();
  }

  // Cached lookup on the def of a ref (like resolve_member) except that if
  // the member is not renamed in the name_mapping, we return nullptr
  T get_def_if_renamed(T member_ref) {
    auto itr = ref_def_cache.find(member_ref);
    if (itr != ref_def_cache.end())
      return itr->second;
    T def = name_mapping.def_of_ref(member_ref);
    ref_def_cache[member_ref] = def;
    return def;
  }
};

// Static state of the renamer
template <class T>
class RenamingContext {
public:
  const std::list<T>& elems;
  const std::unordered_set<std::string>& ids_to_avoid;
  const bool operateOnPrivates;
  NameGenerator<T>* name_gen;

  RenamingContext(std::list<T>& elems,
      std::unordered_set<std::string>& ids_to_avoid,
      NameGenerator<T>* name_gen,
      bool operateOnPrivates) :
      elems(elems), ids_to_avoid(ids_to_avoid),
      operateOnPrivates(operateOnPrivates), name_gen(name_gen) { }

  // Whether or not on this pass we should rename the member
  bool can_rename_elem(T elem) const {
    return should_rename_elem(elem) && operateOnPrivates == is_private(elem);
  }
};
