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
 * Allows us to wrap Dex elements to add extra information to them
 */
template <class T>
class DexElemWrapper {
private:
  T dex_elem;

public:
  // Default constructor is only ever used for map template to work correctly
  // we have added asserts to make sure there are no nullptrs returned.
  DexElemWrapper() = default;
  DexElemWrapper(DexElemWrapper&& other) = default;
  DexElemWrapper(DexElemWrapper const& other) = default;
  explicit DexElemWrapper(T dex_elem) : dex_elem(dex_elem) { }

  DexElemWrapper& operator=(DexElemWrapper const& other) = default;
  DexElemWrapper& operator=(DexElemWrapper&& other) = default;

  inline T get() { always_assert(dex_elem != nullptr); return dex_elem; }
  inline const T get() const { always_assert(dex_elem != nullptr); return dex_elem; }
};

typedef DexElemWrapper<DexField*> DexFieldWrapper;
typedef DexElemWrapper<DexMethod*> DexMethodWrapper;

class FieldNameWrapper : public DexFieldWrapper {
private:
  // the new name that we're trying to give this element
  bool has_new_name{false};
  std::string name{"INVALID_DEFAULT_NAME"};

public:
  FieldNameWrapper() = default;
  explicit FieldNameWrapper(DexField* dex_elem) : DexFieldWrapper(dex_elem) { }

  inline bool name_has_changed() const { return has_new_name; }

  const char* get_name() const {
    return has_new_name ? this->name.c_str() : this->get()->get_name()->c_str();
  }

  void set_name(const std::string& new_name) {
    has_new_name = true;
    name = new_name;
  }
};

/*
 * Interface for factories for new obfuscated names.
 */
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
  std::string next_name();

public:
  NameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>* used_ids) :
      ids_to_avoid(ids_to_avoid), used_ids(used_ids) {}
  virtual ~NameGenerator() = default;

  // Notes that we want to rename the DexField pointed to by this wrapper.
  // our new name will be recorded in the wrapper
  virtual void find_new_name(FieldNameWrapper* wrap) = 0;

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
class SimpleNameGenerator : public NameGenerator {
public:
  SimpleNameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      std::unordered_set<std::string>* used_ids) :
    NameGenerator(ids_to_avoid, used_ids) {}

  void find_new_name(FieldNameWrapper* wrap) override {
    std::string new_name(next_name());
    wrap->set_name(new_name);
    used_ids->insert(new_name);
    DexField* field = wrap->get();
    TRACE(OBFUSCATE, 2, "\tIntending to rename field (%s) %s:%s to %s\n",
        SHOW(field->get_type()), SHOW(field->get_class()),
        SHOW(field->get_name()), new_name.c_str());
  }
};

// Will collect all the wrappers of fields to rename then rename them all at
// once making sure to put static final fields with a nullptr value at the end
// (for proper writing of dexes)
class StaticNameGenerator : public NameGenerator {
private:
  std::unordered_set<FieldNameWrapper*> fields;
  std::unordered_set<FieldNameWrapper*> static_final_null_fields;
public:
  StaticNameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
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
        std::string new_name(next_name());
        wrap->set_name(new_name);
        used_ids->insert(new_name);
      }
      return;
    }
    // Otherwise we have to make sure that all the names are assigned in order
    // such that the static final null fields are at the end
    std::vector<std::string> names;
    for (unsigned int i = 0;
        i < fields.size() + static_final_null_fields.size(); ++i)
      names.emplace_back(next_name());
    // if we have more than 52 names, next_name won't be in alphabetical order
    // because it'll return "AA" after "z"
    std::sort(names.begin(), names.end());
    TRACE(OBFUSCATE, 3, "Static Generator\n");
    unsigned int i = 0;
    for (FieldNameWrapper* wrap : fields) {
      std::string new_name(names[i++]);
      wrap->set_name(new_name);
      used_ids->insert(new_name);
      DexField* field = wrap->get();
      TRACE(OBFUSCATE, 3, "\tIntending to rename field (%s) %s:%s to %s\n",
          SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), new_name.c_str());
    }
    for (FieldNameWrapper* wrap : static_final_null_fields) {
      std::string new_name(names[i++]);
      wrap->set_name(new_name);
      used_ids->insert(new_name);
      DexField* field = wrap->get();
      TRACE(OBFUSCATE, 3,
          "\tIntending to rename static null field (%s) %s:%s to %s\n",
          SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), new_name.c_str());
    }
  }

  void reset() override {
    NameGenerator::reset();
    fields.clear();
    static_final_null_fields.clear();
  }
};

class DexFieldManager {
private:
  // Map from class_name -> type -> old_name ->
  //   DexField wrapper (contains new name)
  std::unordered_map<DexType*,
    std::unordered_map<DexType*,
      std::unordered_map<DexString*, FieldNameWrapper>>> elements;

public:
  inline bool contains_field(
      DexType* cls, DexType* type, DexString* name) {
    return elements.count(cls) > 0 &&
      elements[cls].count(type) > 0 &&
      elements[cls][type].count(name) > 0;
  }

  inline bool contains_field(DexField*& elem) {
    return contains_field(elem->get_class(),
      elem->get_type(), elem->get_name());
  }

  // Mirrors the map get operator, but ensures we create correct wrappers
  // if they don't exist
  FieldNameWrapper& operator[](DexField*& elem) {
    return contains_field(elem) ?
      elements[elem->get_class()]
        [elem->get_type()][elem->get_name()] :
      elements[elem->get_class()][elem->get_type()].emplace(
        elem->get_name(), FieldNameWrapper(elem)).first->second;
  }

  // Commits all the renamings in elements to the dex by modifying the
  // underlying DexFields. Does in-place modification.
  void commit_renamings_to_dex();

  // Does a lookup over the fields we renamed in the dex to see what the
  // reference should be reset with. Returns kSouldNotReset if there is no
  // mapping.
  // Note: we also have to look in superclasses in the case that this is a ref
  DexField* def_of_ref(DexField* ref);

  // Debug print of the mapping
  void print_elements();
};

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

// Whether or not the configs allow for us to obfuscate the field
inline bool should_rename_field(const DexField* member) {
  return !is_seed(member) && (!keep(member) || allowobfuscation(member));
}

inline bool should_rename_method(const DexMethod* member) {
  return !is_seed(member) && (!keep(member) || allowobfuscation(member));
}

// Look at a list of fields and check if there is a renamable field in the list
bool contains_renamable_field(const std::list<DexField*>& fields);

// Used to collect all conflicting names
class FieldNameCollector : public MemberVisitor {
private:
  std::unordered_set<std::string>* names;
  DexFieldManager* field_name_mapping;

public:
  FieldNameCollector(
      DexFieldManager* field_name_mapping,
      std::unordered_set<std::string>* names) :
        names(names), field_name_mapping(field_name_mapping) { }
  virtual ~FieldNameCollector() = default;

  virtual void visit_field(DexField* f) override {
    TRACE(OBFUSCATE, 4, "Visiting field %s\n", SHOW(f));
    names->insert((*field_name_mapping)[f].get_name());
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

// State of the renaming that we need to modify as we rename more fields
class ObfuscationState {
  // Cache of ref -> def mapping in case we have many instructions that
  // use the same ref.
  std::unordered_map<DexField*, DexField*> ref_def_cache;
public:
  DexFieldManager name_mapping;
  std::unordered_set<std::string> used_ids;

  void commit_renamings_to_dex() {
    name_mapping.commit_renamings_to_dex();
  }

  // Cached lookup on the def of a ref (like resolve_field) except that if
  // the field is not renamed in the name_mapping, we return nullptr
  DexField* get_def_if_renamed(DexField* field_ref);
};

// Static state of the renamer
class RenamingContext {
public:
  const std::list<DexField*>& fields;
  const std::unordered_set<std::string>& ids_to_avoid;
  const bool operateOnPrivates;
  NameGenerator* name_gen;

  RenamingContext(std::list<DexField*>& fields,
      std::unordered_set<std::string>& ids_to_avoid,
      NameGenerator* name_gen,
      bool operateOnPrivates) :
      fields(fields), ids_to_avoid(ids_to_avoid),
      operateOnPrivates(operateOnPrivates), name_gen(name_gen) { }

  // Whether or not on this pass we should rename the field
  bool can_rename_field(DexField* field) const {
    return should_rename_field(field) && operateOnPrivates == is_private(field);
  }
};
