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
class IdFactory {
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
  IdFactory(const std::unordered_set<std::string>& ids_to_avoid,
    const std::unordered_set<std::string>& used_ids) :
    ids_to_avoid(ids_to_avoid), used_ids(used_ids) {
      TRACE(OBFUSCATE, 1, "Created new IdFactory\n");
    }

  // Gets the next name that is not in the used_ids set
  std::string next_name();

  // If we didn't use a name, we can roll back the counter
  inline void rollback() { ctr -= 1; }

  inline void reset() {
    TRACE(OBFUSCATE, 1, "Resetting factory\n");
    ctr = 1;
  }
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
public:
  ClassVisitor(VisitFilter vf) : visit_filter(vf) { }
  virtual ~ClassVisitor() {}
  virtual void visit(DexClass*) = 0;

  inline bool should_visit_private() {
    return visit_filter & VisitFilter::PrivateOnly;
  }

  inline bool should_visit_public() {
    return visit_filter & VisitFilter::NonPrivateOnly;
  }
};

inline bool should_rename_field(const DexField* field) {
  return !keep(field) || allowobfuscation(field);
}

// Used to collect all conflicting names
class FieldVisitor : public ClassVisitor {
private:
  std::unordered_set<std::string>* names;
public:

  FieldVisitor(std::unordered_set<std::string>* names,
    VisitFilter visit_filter) : ClassVisitor(visit_filter), names(names) { }

  inline void visit_field(DexField* f) {
    if ((should_visit_private() && is_private(f)) ||
        (should_visit_public() && !is_private(f)))
      names->insert(f->get_name()->c_str());
  }

  void visit(DexClass* cls) override;
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
