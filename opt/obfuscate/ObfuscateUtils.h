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

constexpr int kMaxIdentChar (52);

// Type for the map of descriptor -> [newname -> oldname]
// This map is used for reverse lookup to find naming collisions
typedef std::unordered_map<std::string,
    std::unordered_map<std::string, std::string>> NameMapping;

/*
 * Factory for new obfuscated names.
 */
class NameGenerator {
private:
  int ctr{1};
  inline char get_ident(int num) {
    return num > 26 ? 'A' + num : 'a' + num;
  }
  // Set of ids to avoid (these ids were marked as do not rename and we cannot
  // conflict with)
  const std::unordered_set<std::string>& ids_to_avoid;
  // Set of ids we used while assigning names
  const std::unordered_set<std::string>& used_ids;

public:
  NameGenerator(const std::unordered_set<std::string>& ids_to_avoid,
      const std::unordered_set<std::string>& used_ids) :
      ids_to_avoid(ids_to_avoid), used_ids(used_ids) {
    TRACE(OBFUSCATE, 2, "Created new NameGenerator\n");
  }

  // Gets the next name that is not in the used_ids set
  std::string next_name();

  // If we didn't use a name, we can roll back the counter
  inline void rollback() { ctr -= 1; }

  inline void reset() {
    TRACE(OBFUSCATE, 2, "Resetting generator\n");
    ctr = 1;
  }
};

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

  inline T get() { assert(dex_elem != nullptr); return dex_elem; }
  inline const T get() const { assert(dex_elem != nullptr); return dex_elem; }
};

typedef DexElemWrapper<DexField*> DexFieldWrapper;
typedef DexElemWrapper<DexMethod*> DexMethodWrapper;

class FieldNameWrapper : public DexFieldWrapper {
private:
  // the new name that we're trying to give this element
  bool has_new_name{false};
  std::string name;

public:
  FieldNameWrapper() = default;
  explicit FieldNameWrapper(DexField* dex_elem) : DexFieldWrapper(dex_elem) {
    has_new_name = true;
  }

  const char* get_name() const {
    return has_new_name ? this->name.c_str() : this->get()->get_name()->c_str();
  }

  void set_name(const std::string& new_name) {
    has_new_name = true;
    name = new_name;
  }
};

class DexFieldManager {
private:
  std::unordered_map<DexField*, FieldNameWrapper> elements;

public:
  inline bool contains_field(DexField*& elem) {
    return elements.count(elem) > 0;
  }

  // Mirrors the map get operator
  FieldNameWrapper& operator[](DexField*& elem) {
    return contains_field(elem) ? elements[elem] :
        elements.emplace(elem, FieldNameWrapper(elem)).first->second;
  }

  // Debug print of the mapping
  void print() {
    TRACE(OBFUSCATE, 4, "Field Ptr: old name -> new name\n");
    for (auto& ptr_nwrap : elements)
      TRACE(OBFUSCATE, 4, "0x%x: %s -> %s\n",
          ptr_nwrap.first,
          SHOW(ptr_nwrap.second.get()),
          ptr_nwrap.second.get_name());
  }
};

class MemberVisitor {
public:
  inline void visit_field(DexField* /* unused */) {}
  inline void visit_method(DexMethod* /* unused */) {}
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
  MemberVisitor member_visitor;
public:
  ClassVisitor(VisitFilter vf, MemberVisitor mv) :
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
  return !keep(member) || allowobfuscation(member);
}

inline bool should_rename_method(const DexMethod* member) {
  return !keep(member) || allowobfuscation(member);
}

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

  void visit_field(DexField* f) {
    TRACE(OBFUSCATE, 3, "Visiting field %s\n", SHOW(f));
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
public:
  DexFieldManager name_mapping;
  std::unordered_set<std::string> used_ids;

  void set_name_mapping(DexField* field, const std::string& name) {
    name_mapping[field].set_name(name);
    used_ids.insert(name);
  }
};

// Static state of the renamer
class RenamingContext {
public:
  const std::list<DexField*>& fields;
  const std::unordered_set<std::string>& ids_to_avoid;
  const bool operateOnPrivates;

  RenamingContext(std::list<DexField*>& fields,
      std::unordered_set<std::string>& ids_to_avoid,
      bool operateOnPrivates) :
      fields(fields), ids_to_avoid(ids_to_avoid),
      operateOnPrivates(operateOnPrivates) { }

  // Whether or not on this pass we should rename the field
  bool can_rename_field(DexField* field) const {
    return should_rename_field(field) && operateOnPrivates == is_private(field);
  }
};
